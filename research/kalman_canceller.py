"""Beat-bleed canceller, version 2: partitioned-block frequency-domain adaptive KALMAN filter (Enzner & Vary,
Signal Processing 86 (2006); partitioned form: Kuech, Mabande & Enzner, IEEE/ACM TASLP 22 (2014)), diagonalised per
frequency bin, two references (left and right headphone cups).

State: the bleed path W_rp(k) of every reference r and partition p, with its uncertainty P_rp(k) (a scalar per bin).
Model: W <- A W + process noise (A = 0.9995 per block: the path drifts as the singer moves);
       mic block = sum_rp X_rp W_rp + everything else (voice, breaths, noise), whose power Psi_S(k) is estimated online.
The Kalman gain K = P X* / (sum |X|^2 P + Psi_S) is the optimal step size: where the voice, a breath or the noise
dominates a bin, Psi_S is large and that bin barely adapts - no double-talk detector, no hand-tuned step."""
import numpy as np

class KalmanCanceller:
    def __init__(self, fs=48000, block=256, taps_ms=60.0, lead_ms=8.0, n_ref=2, A=0.9995, init_var=1e-3, psi_floor=0.05):
        self.B = block; self.N = 2 * block
        self.P_ = int(np.ceil(taps_ms * 1e-3 * fs / block))
        self.lead = int(round(lead_ms * 1e-3 * fs))
        self.n_ref = n_ref; self.A = A
        K = block + 1
        self.W = np.zeros((n_ref, self.P_, K), complex)
        self.X = np.zeros((n_ref, self.P_, K), complex)
        self.Pv = np.full((n_ref, self.P_, K), init_var)        # state uncertainty per bin
        self.xbuf = np.zeros((n_ref, self.N))
        self.PsiS = None
        self.psi_floor = psi_floor

    def process(self, mic, ref):
        B, N, P_ = self.B, self.N, self.P_
        n = len(mic) // B * B
        mic_d = np.concatenate([np.zeros(self.lead), mic])[:len(mic)]
        out = np.zeros(n); est = np.zeros(n)
        A2 = self.A ** 2
        for s in range(0, n, B):
            for r in range(self.n_ref):
                self.xbuf[r] = np.concatenate([self.xbuf[r, B:], ref[s:s + B, r]])
                self.X[r] = np.roll(self.X[r], 1, axis=0)
                self.X[r, 0] = np.fft.rfft(self.xbuf[r])
            # prediction (time update): the path may drift; its uncertainty grows by the process noise
            self.W *= self.A
            self.Pv = A2 * self.Pv + (1 - A2) * np.abs(self.W) ** 2 + 1e-12
            Y = np.sum(self.W * self.X, axis=(0, 1))
            y = np.fft.irfft(Y)[B:]
            e = mic_d[s:s + B] - y
            out[s:s + B] = e; est[s:s + B] = y
            E = np.fft.rfft(np.concatenate([np.zeros(B), e]))
            # measurement noise (everything that is not bleed) per bin: the error power minus what the remaining
            # path uncertainty explains, smoothed; floored
            Xp = np.abs(self.X) ** 2
            unc = np.sum(Xp * self.Pv, axis=(0, 1))
            # never below a share of the error itself: an over-estimated uncertainty must not turn into a huge step
            psi = np.maximum(np.abs(E) ** 2 - unc, self.psi_floor * np.abs(E) ** 2 + 1e-12)
            self.PsiS = psi if self.PsiS is None else 0.8 * self.PsiS + 0.2 * psi
            # Kalman gain and update (diagonal approximation; overlap-save scaling B/N on the measurement noise)
            denom = unc + self.PsiS * (N / B) * 0.5 + 1e-20
            for r in range(self.n_ref):
                for p in range(P_):
                    Kg = self.Pv[r, p] * np.conj(self.X[r, p]) / denom
                    w = np.fft.irfft(self.W[r, p] + Kg * E); w[B:] = 0.0          # gradient constraint (causal taps)
                    self.W[r, p] = np.fft.rfft(w)
                    self.Pv[r, p] = np.maximum((1 - 0.5 * np.real(Kg * self.X[r, p])) * self.Pv[r, p], 1e-14)
        return out, est
