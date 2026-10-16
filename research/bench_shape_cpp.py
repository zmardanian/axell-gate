"""SHAPE mode through the C++ engine: decay scaling vs the classic gate release tuned to the same T40, per hit type.
usage: python3 bench_shape_cpp.py <GateRender binary> [tail%]"""
import os, sys, subprocess, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shape_sim as SS
from bench_vocal_cpp import wavf
TOOL = sys.argv[1]; TAIL = float(sys.argv[2]) if len(sys.argv) > 2 else 50.0
TMP = os.path.expanduser("~/axell-gate-data/bench")
H = SS.hits(); pad = int(0.05 * SS.FS)
print(f"{'hit':6s} {'law':10s} {'T40 s':>6s} {'splatter dB':>12s} {'attack err dB':>14s} {'kink dB':>8s}")
for name, h in H.items():
    x = np.concatenate([np.zeros(pad), h, np.zeros(int(0.3 * SS.FS))]) + 10 ** (-80 / 20) * np.random.default_rng(1).standard_normal(pad + len(h) + int(0.3 * SS.FS))
    wavf(f"{TMP}/h.wav", x)
    r = subprocess.run([TOOL, f"{TMP}/h.wav", f"{TMP}/ho.wav", "mode=1", f"tail={TAIL}", "threshold=-60", "range=-80", "hold=0", "release=200", "attack=1"], capture_output=True, text=True, check=True)
    import wave
    from wav24 import read_wav  # noqa
    ya = np.fromfile(f"{TMP}/ho.wav", np.float32)[11:][:len(x)].astype(np.float64)   # float WAV, 44-byte header
    onset = pad
    ta = SS.t40(ya, onset)
    best = None
    for thr in (-50, -40, -30, -20):
        for rel in np.geomspace(0.005, 1.5, 50):
            yc = SS.classic(x, onset, thr_db=thr, release_s=rel)
            tc = SS.t40(yc, onset)
            if best is None or abs(tc - ta) < abs(best[0] - ta):
                best = (tc, yc)
    for law, y, tt in (("natural", x, SS.t40(x, onset)), ("Axell C++", ya, ta), ("classic", best[1], best[0])):
        _, att, kink = SS.metrics(x, y, onset)
        sp = SS.splatter(x, y, onset) if law != "natural" else -np.inf
        print(f"{name:6s} {law:10s} {tt:6.3f} {sp:12.1f} {att:14.1f} {kink:8.2f}")
