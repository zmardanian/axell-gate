"""Beat-bleed canceller prototype: a two-channel partitioned-block frequency-domain adaptive filter (PBFDAF,
overlap-save) from the beat on the side-chain (L, R) to the vocal mic, with a per-bin optimal step size in the spirit of
Valin (IEEE TASLP 2007, "On adjusting the learning rate in frequency domain echo cancellation with double-talk"):
mu_k = min(mu_max, leak * |Y_k|^2 / |E_k|^2) - the filter learns fast while the error is mostly unexplained echo and
almost stops while the voice dominates (double talk), without a hard double-talk detector.

The mic path is delayed by `lead` samples so the filter can also model bleed that arrives *before* the reference
(recording offset between the vocal and the beat track)."""
import numpy as np

class Canceller:
    def __init__(self, fs=48000, block=256, taps_ms=60.0, lead_ms=8.0, mu_max=0.5, n_ref=2):
        self.B = block; self.N = 2 * block
        self.P = int(np.ceil(taps_ms * 1e-3 * fs / block))            # partitions
        self.lead = int(round(lead_ms * 1e-3 * fs))
        self.n_ref = n_ref; self.mu_max = mu_max
        self.W = np.zeros((n_ref, self.P, block + 1), complex)          # filter, frequency domain
        self.X = np.zeros((n_ref, self.P, block + 1), complex)          # reference spectra history
        self.xbuf = np.zeros((n_ref, self.N))
        self.leak = 0.5                                                  # leakage estimate (how much of the error is echo)
        self.Pe = np.ones(block + 1) * 1e-6; self.Py = np.ones(block + 1) * 1e-6
        self.Pyy = 1e-9; self.Pey = 0.0; self.Pee = 1e-9
        self.Px = np.zeros(block + 1)

    def process(self, mic, ref, adapt=None):
        """mic: (n,), ref: (n, n_ref). Returns the mic with the estimated bleed removed and the estimate (both delayed by
        `lead`; the reference is used as is - so the filter sees `lead` samples of the future of the mic)."""
        B, N = self.B, self.N
        n = len(mic) // B * B
        mic_d = np.concatenate([np.zeros(self.lead), mic])[:len(mic)]
        out = np.zeros(n); est = np.zeros(n)
        for s in range(0, n, B):
            for r in range(self.n_ref):
                self.xbuf[r] = np.concatenate([self.xbuf[r, B:], ref[s:s + B, r]])
                self.X[r] = np.roll(self.X[r], 1, axis=0)
                self.X[r, 0] = np.fft.rfft(self.xbuf[r])
            Y = np.sum(self.W * self.X, axis=(0, 1))
            y = np.fft.irfft(Y)[B:]
            e = mic_d[s:s + B] - y
            out[s:s + B] = e; est[s:s + B] = y
            E = np.fft.rfft(np.concatenate([np.zeros(B), e]))
            # leakage estimate: regression of |E|^2 on |Y|^2 over time (how much of the error power follows the echo)
            Ye = np.abs(np.fft.rfft(np.concatenate([np.zeros(B), y]))) ** 2; Ee = np.abs(E) ** 2
            dY = Ye - self.Py; dE = Ee - self.Pe
            self.Py += 0.05 * (Ye - self.Py); self.Pe += 0.05 * (Ee - self.Pe)
            self.Pey = 0.98 * self.Pey + 0.02 * np.sum(dE * dY); self.Pyy = 0.98 * self.Pyy + 0.02 * np.sum(dY * dY)
            self.leak = float(np.clip(self.Pey / max(self.Pyy, 1e-30), 0.005, 1.0))
            if adapt is not None:
                # externally controlled (voice activity): learn in the pauses, nearly freeze under the voice
                a = float(np.mean(adapt[s:s + B]))
                mu = np.full(B + 1, self.mu_max * a + 0.002 * (1 - a))
            else:
                mu = np.minimum(self.mu_max, self.leak * Ye / (Ee + 1e-12))
                # early phase: plain NLMS until the filter produces an estimate at all
                if np.sum(Ye) < 1e-14:
                    mu = np.full_like(mu, 0.2)
            # normalised update: the filter's whole input power per bin (all references, all partitions) - exact,
            # so a sudden kick cannot overshoot the step
            Px = np.sum(np.abs(self.X) ** 2, axis=(0, 1)) + 1e-10
            for r in range(self.n_ref):
                G = (mu / Px) * E
                for p in range(self.P):
                    upd = np.conj(self.X[r, p]) * G
                    w = np.fft.irfft(self.W[r, p] + upd); w[B:] = 0.0           # gradient constraint
                    self.W[r, p] = np.fft.rfft(w)
        return out, est
