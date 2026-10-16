"""2.0 real-take check: a real dry vocal (no labels) through GateRender. What can be measured without labels:
  GAPS   - stretches 150 ms or more from any frame within 30 dB of the voice's level (true silence between lines):
           how much of that time the gate is open (gain above -6 dB) and the mean gain there;
  VOICE  - the energy the gate removes from the whole take, 10 log10(sum((1-g) x)^2 / sum x^2) (the gaps hold
           almost none of it, so it is the damage to the voice), and the energy-weighted damage to word starts;
compared with the classic gate (gate_sim.classic_gain) tuned over its grid on this very take, at least as closed in
the gaps. usage: python3 bench_real.py <GateRender> <take.wav> [key=value ...]"""
import os, sys, itertools, numpy as np
from scipy import signal as sg
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_sim as G, subprocess
from wav24 import read_wav
from bench_vocal_cpp import wavf, WORK as TMP

def regions(x, fs):
    hp = sg.sosfilt(sg.butter(4, 60, "high", fs=fs, output="sos"), x)
    fr = int(0.01 * fs); n = len(hp) // fr
    lev = 10 * np.log10(np.mean(hp[:n * fr].reshape(n, fr) ** 2, 1) + 1e-20)
    vref = np.percentile(lev, 95)
    near = lev > vref - 30
    far = sg.convolve(near.astype(float), np.ones(31), mode="same") == 0      # +-150 ms
    word = lev > vref - 20
    start = np.zeros(n, bool)
    for a, e in G.segments(word):
        start[a:min(e, a + 4)] = True                                            # first 40 ms of each loud stretch
    up = lambda m: np.repeat(m, fr)[:len(x)] if len(x) <= n * fr else np.concatenate([np.repeat(m, fr), np.zeros(len(x) - n * fr, bool)])
    return up(far), up(start), vref

def metrics(g, x, gap, start):
    db = lambda a, b: 10 * np.log10((a + 1e-30) / (b + 1e-30))
    return dict(open=100 * np.mean(g[gap] > 0.5), gapdb=20 * np.log10(np.mean(g[gap]) + 1e-9),
                voice=db(np.sum(((1 - g) * x) ** 2), np.sum(x ** 2)),
                starts=db(np.sum(((1 - g) * x)[start] ** 2), np.sum(x[start] ** 2)))

if __name__ == "__main__":
    tool, path = sys.argv[1], sys.argv[2]
    fs, X = read_wav(path); x = X.mean(1) if X.ndim > 1 else X
    assert fs == 48000, "classic_gain assumes 48 kHz"
    gap, start, vref = regions(x, fs)
    os.makedirs(TMP, exist_ok=True)
    wavf(f"{TMP}/real.wav", x)
    subprocess.run([tool, f"{TMP}/real.wav", f"{TMP}/real_o.wav", f"gainout={TMP}/real_g.f32"] + sys.argv[3:], check=True, capture_output=True)
    g = np.fromfile(f"{TMP}/real_g.f32", np.float32).astype(np.float64)[:len(x)]
    g = np.nan_to_num(g, nan=1.0)
    m = metrics(g, x, gap, start)
    print(f"{os.path.basename(path)}: {len(x) / fs:.1f} s, gaps {100 * gap.mean():.0f} % of the take, voice level {vref:.1f} dBFS (10 ms, 95th pct)")
    print(f"  Axell: open in gaps {m['open']:5.1f} % (mean gain {m['gapdb']:6.1f} dB) | voice removed {m['voice']:6.1f} dB | word starts {m['starts']:6.1f} dB")
    flo = 10 * np.log10(np.mean(x[gap] ** 2) + 1e-20)
    best = None
    for thr, look, hold, rel in itertools.product(np.arange(flo + 3, flo + 40, 3), (0, 2, 5, 10), (20, 80, 200), (40, 120, 300)):
        gc = G.classic_gain(x, thr, look_ms=look, hold_ms=hold, release_ms=rel)[:len(x)]
        if len(gc) < len(x):
            gc = np.concatenate([gc, np.full(len(x) - len(gc), gc[-1])])
        c = metrics(gc, x, gap, start)
        if c["gapdb"] <= m["gapdb"] + 0.5 and (best is None or c["voice"] + 0.5 * c["starts"] < best[0]["voice"] + 0.5 * best[0]["starts"]):
            best = (c, (thr - flo, look, hold, rel))
    if best:
        c = best[0]
        print(f"  classic tuned on this take, at least as closed: open {c['open']:5.1f} % (mean {c['gapdb']:6.1f} dB) | voice removed {c['voice']:6.1f} dB | "
              f"word starts {c['starts']:6.1f} dB  (thr gaps+{best[1][0]:.0f}, look {best[1][1]}, hold {best[1][2]}, rel {best[1][3]})")
    else:
        print("  classic: no setting is as closed in the gaps")
