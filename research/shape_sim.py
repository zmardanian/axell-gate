"""SHAPE mode prototype and benchmark: shortening drum tails.

CLASSIC gate: level threshold, look-ahead, attack, hold, then a release ramp (linear in dB, the usual "musical" law)
to the floor once the level drops below the threshold.

AXELL decay scaling: after each hit's onset (found with look-ahead, sample-accurate), the gain follows the hit's own
envelope: g = (e / e_peak)^alpha. An exponential tail exp(-t/tau) comes out as exp(-(1+alpha) t / tau): the same shape,
(1 + alpha) times faster - no kink where a release starts, no ripple (e is the magnitude of the analytic signal, smooth
even on a sine 808). TAIL = 1 / (1 + alpha) (100 % natural, 50 % twice as fast).

Metrics per hit type, the classic release tuned to the same T40 (time to -40 dB re the peak):
  click   new energy above 6 kHz created in the tail (gain-modulation splatter), re the hit's energy
  attack  error in the first 3 ms after the onset, re the energy there (a chopped or late-opening transient)
  kink    rms deviation (dB) of the output's log-envelope from a straight decay over the tail (a natural decay is
          straight in dB; a release that starts mid-decay bends it)"""
import numpy as np
from scipy import signal as sg

FS = 48000

def hits():
    """isolated programmed drum hits: kick, snare, 808 (sine, long), clap-like noise burst, tom"""
    t = lambda s: np.arange(int(s * FS)) / FS
    rng = np.random.default_rng(3)
    out = {}
    tt = t(0.6); f = 45 + 110 * np.exp(-tt * 35)
    out["kick"] = np.sin(2 * np.pi * np.cumsum(f) / FS) * np.exp(-tt * 7) + 0.25 * np.exp(-tt * 400) * rng.standard_normal(len(tt))
    tt = t(0.6)
    nz = sg.lfilter(*sg.butter(2, [1500, 9000], "bandpass", fs=FS), rng.standard_normal(len(tt)))
    out["snare"] = 0.8 * nz * np.exp(-tt * 9) + 0.5 * np.sin(2 * np.pi * 185 * tt) * np.exp(-tt * 20)
    tt = t(2.0); f = 49 * (1 + 0.12 * np.exp(-tt * 18))
    out["808"] = np.sin(2 * np.pi * np.cumsum(f) / FS) * np.exp(-tt * 1.6)
    tt = t(0.5)
    nz = sg.lfilter(*sg.butter(2, [800, 6000], "bandpass", fs=FS), rng.standard_normal(len(tt)))
    out["clap"] = nz * np.exp(-tt * 12)
    tt = t(1.0); f = 110 * (1 + 0.2 * np.exp(-tt * 25))
    out["tom"] = np.sin(2 * np.pi * np.cumsum(f) / FS) * np.exp(-tt * 5)
    for k in out:
        out[k] = out[k] / np.max(np.abs(out[k])) * 0.7
    return out

def analytic_env(x):
    return np.abs(sg.hilbert(x))

def t40(y, onset):
    e = sg.lfilter([1 - 0.995], [1, -0.995], y ** 2)
    edb = 10 * np.log10(e + 1e-30); pk = edb[onset:onset + int(0.02 * FS)].max()
    below = np.where(edb[onset + int(0.02 * FS):] < pk - 40)[0]
    return (below[0] + int(0.02 * FS)) / FS if len(below) else len(y) / FS

def classic(x, onset, thr_db=-40, release_s=0.1, look=int(0.002 * FS), attack=int(0.0005 * FS), hold_s=0.0, rng_db=-80):
    """gate open from (onset - look) while the level is above the threshold (+hold), then a release linear in dB"""
    e = sg.lfilter([1 - 0.999], [1, -0.999], x ** 2); edb = 10 * np.log10(e + 1e-30)
    n = len(x); g_db = np.full(n, rng_db)
    above = np.where(edb > thr_db)[0]
    end = (above[-1] if len(above) else onset) + int(hold_s * FS)
    a0 = max(0, onset - look)
    g_db[a0:a0 + attack] = np.linspace(rng_db, 0, attack)
    g_db[a0 + attack:end] = 0.0
    rl = int(release_s * FS)
    g_db[end:end + rl] = np.linspace(0, rng_db, len(g_db[end:end + rl]))
    return x * 10 ** (g_db / 20)

def axell(x, onset, tail=0.5, rng_db=-80, ramp=int(0.0005 * FS), smooth_ms=1.0):
    alpha = 1.0 / tail - 1.0
    e = analytic_env(x)
    k = np.exp(-1 / (smooth_ms * 1e-3 * FS))
    e = sg.lfilter([1 - k], [1, -k], e)                    # light smoothing of the analytic envelope (broadband hits)
    pk_i = onset + np.argmax(e[onset:onset + int(0.02 * FS)])
    epk = e[pk_i]
    g = np.ones(len(x))
    g[pk_i:] = np.clip((e[pk_i:] / epk) ** alpha, 10 ** (rng_db / 20), 1.0)
    # never rising again after the peak (a monotone fade): running minimum
    g[pk_i:] = np.minimum.accumulate(g[pk_i:])
    g[:max(0, onset - ramp)] = 10 ** (rng_db / 20)
    a0 = max(0, onset - ramp)
    g[a0:onset] = 10 ** (rng_db / 20) + (1 - 10 ** (rng_db / 20)) * (0.5 - 0.5 * np.cos(np.pi * np.arange(onset - a0) / max(onset - a0, 1)))
    return x * g

def metrics(x, y, onset):
    n = len(x)
    tail = slice(onset + int(0.02 * FS), n)
    hp = sg.butter(4, 6000, "high", fs=FS, output="sos")
    click = 10 * np.log10(np.sum(sg.sosfilt(hp, y - x * (np.abs(y) > 0) * 0 - 0)[tail] ** 2 + 0) + 1e-30)
    # new HF energy: HF of the output minus what the input had there (scaled by the gain is ambiguous; compare the
    # output's HF to the HF of a perfectly smooth version: x times the gain's 1 ms moving average)
    on = slice(onset, onset + int(0.003 * FS))
    att = 10 * np.log10(np.sum((y - x)[on] ** 2) / np.sum(x[on] ** 2) + 1e-30)
    e = sg.lfilter([1 - 0.99], [1, -0.99], y ** 2); edb = 10 * np.log10(e + 1e-30)
    pk = edb[onset:onset + int(0.02 * FS)].max()
    seg = np.where((edb > pk - 45) & (np.arange(n) > onset + int(0.02 * FS)))[0]
    kink = np.nan
    if len(seg) > 10:
        tt = seg / FS; c = np.polyfit(tt, edb[seg], 1); kink = np.sqrt(np.mean((edb[seg] - np.polyval(c, tt)) ** 2))
    return click, att, kink

def splatter(x, y, onset):
    """HF energy (> 6 kHz) of the output in the tail that the input did not have at that point: modulation splatter,
    re the hit's total energy (dB)"""
    hp = sg.butter(4, 6000, "high", fs=FS, output="sos")
    hx = sg.sosfilt(hp, x); hy = sg.sosfilt(hp, y)
    tail = slice(onset + int(0.02 * FS), len(x))
    # the output's HF beyond the input's HF scaled by the local gain envelope (|y|/|x| smoothed)
    g = np.abs(y) / (np.abs(x) + 1e-12)
    gs = sg.medfilt(g[::48], 5).repeat(48)[:len(x)]
    new = hy - hx * gs
    return 10 * np.log10(np.sum(new[tail] ** 2) / np.sum(x ** 2) + 1e-30)

if __name__ == "__main__":
    H = hits()
    pad = int(0.05 * FS)
    print(f"{'hit':6s} {'law':8s} {'T40 s':>6s} {'splatter dB':>12s} {'attack err dB':>14s} {'kink dB':>8s}")
    for name, h in H.items():
        x = np.concatenate([np.zeros(pad), h, np.zeros(int(0.2 * FS))]) + 10 ** (-80 / 20) * np.random.default_rng(1).standard_normal(pad + len(h) + int(0.2 * FS))
        onset = pad
        nat = t40(x, onset)
        ya = axell(x, onset, tail=0.5)
        ta = t40(ya, onset)
        # classic: search the release (and threshold) that gives the same T40
        best = None
        for thr in (-50, -40, -30, -20):
            for rel in np.geomspace(0.005, 1.0, 40):
                yc = classic(x, onset, thr_db=thr, release_s=rel)
                tc = t40(yc, onset)
                if best is None or abs(tc - ta) < abs(best[0] - ta):
                    best = (tc, yc, thr, rel)
        for law, y, tt in (("natural", x, nat), ("axell", ya, ta), ("classic", best[1], best[0])):
            _, att, kink = metrics(x, y, onset)
            sp = splatter(x, y, onset) if law != "natural" else -np.inf
            print(f"{name:6s} {law:8s} {tt:6.3f} {sp:12.1f} {att:14.1f} {kink:8.2f}")
