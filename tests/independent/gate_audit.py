"""Independent black-box audit of the built Axell Gate VST3 through a real VST3 host (GateAuditHost, JUCE's VST3 hosting
code), with test signals and analysis written here from textbook definitions (numpy/scipy) - nothing uses the plugin's
own code. Exit code = number of failures. Environment: AXELL_AUDIT_HOST, AXELL_VST3, AXELL_AUDIT_WORK."""
import os, sys, time
import numpy as np
from scipy import signal as sg

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
os.environ.setdefault("AXELL_AUDIT_HOST", os.path.join(ROOT, "build/GateAuditHost_artefacts/Release/GateAuditHost"))
os.environ.setdefault("AXELL_VST3", os.path.join(ROOT, "build/AxellGate_artefacts/Release/VST3/Axell Gate.vst3"))
os.environ.setdefault("AXELL_AUDIT_WORK", os.path.expanduser("~/.cache/axell-gate-audit"))
_vendored = os.path.join(ROOT, "external", "axell-core")
CORE = os.environ.get("AXELL_CORE", _vendored if os.path.isdir(_vendored) else os.path.join(os.path.dirname(ROOT), "axell-core"))
sys.path.insert(0, os.path.join(CORE, "tools"))
import lib  # noqa: E402
from lib import Run  # noqa: E402

def run_all(runs, inputs, sidechains=None, **kw):
    f = lambda d: {k: np.asarray(v, np.float32) for k, v in d.items()} if d else d
    return lib.run_all(runs, f(inputs), f(sidechains), **kw)

def f32(x):
    return np.asarray(x, np.float32).astype(np.float64)

results = []
def report(name, ok, detail):
    results.append((name, ok, detail))
    print(f"{'PASS' if ok else 'FAIL'}  {name}: {detail}", flush=True)

def db(a):
    return 20 * np.log10(max(float(a), 1e-30))

def voice(fs, secs, gain_db=-18.0, noise_db=-66.0, seed=21):
    """independent synthetic speech-like signal: glottal pulse train (f0 100-180 Hz) through three formant resonators
    with syllable envelopes and fricative bursts, phrases of 1.4 s separated by 0.9 s pauses, over pink-ish noise.
    Returns (stereo signal, per-sample phrase activity)."""
    rng = np.random.default_rng(seed)
    n = int(secs * fs); t = np.arange(n) / fs
    f0 = 140 + 40 * np.sin(2 * np.pi * 0.31 * t)
    pulses = (np.diff(np.floor(np.cumsum(f0 / fs)), prepend=0) > 0).astype(float)
    src = sg.lfilter([1], [1, -0.96], pulses)
    vow = np.zeros(n)
    for fc, bw, g in ((600, 110, 1.0), (1100, 140, 0.5), (2500, 220, 0.2)):
        r = np.exp(-np.pi * bw / fs); w = 2 * np.pi * fc / fs
        vow += g * sg.lfilter([1 - r], [1, -2 * r * np.cos(w), r * r], src)
    fric = sg.lfilter(*sg.butter(2, [3000, min(9000, 0.45 * fs)], "bandpass", fs=fs), rng.standard_normal(n))
    period = 2.3
    ph = np.mod(t, period)
    active = (ph > 0.3) & (ph < 1.7)
    syl = np.mod(ph, 0.35)
    venv = np.clip(np.minimum(syl, 0.25 - syl) / 0.02, 0, 1) * (syl < 0.25)
    fenv = np.sin(np.pi * np.clip((syl - 0.25) / 0.1, 0, 1)) * (syl >= 0.25)
    x = (vow / np.std(vow) * venv + 0.6 * fric / np.std(fric) * fenv) * active
    x *= 10 ** (gain_db / 20) / np.sqrt(np.mean(x[active] ** 2))
    pink = sg.lfilter([0.049922035, -0.095993537, 0.050612699, -0.004408786], [1, -2.494956002, 2.017265875, -0.522189400], rng.standard_normal(n))
    x += pink / np.std(pink) * 10 ** (noise_db / 20)
    return f32(np.stack([x, x], 1)), active

def aligned(y, L, n):
    return y[L:L + n]

START = time.time()
# ---------------------------------------------------------------- 1. silence; bypass; block sizes
runs, ins = [], {}
for fs in (44100, 48000, 96000):
    for m in ("Vocal", "Shape", "Rhythm"):
        runs.append(Run(f"sil{m}{fs}", fs, sets={"Mode": m})); ins[f"sil{m}{fs}"] = np.zeros((fs, 2))
out = run_all(runs, ins, jobname="sil")
for r in runs:
    report(f"silence in -> exactly silence out ({r.name[3:]})", np.abs(out[r.name]).max() == 0.0, f"max |y| {np.abs(out[r.name]).max():.3g}")
fs = 48000
x, act = voice(fs, 8.0)
runs = [Run("byp", fs, sets={"Bypass": "On"}), Run("b512", fs), Run("brand", fs, blocks="random 1 4096 7")]
out = run_all(runs, {r.name: x for r in runs}, jobname="byp")
y = aligned(out["byp"], runs[0].latency, len(x))
report("host bypass: exactly the input, delayed by the reported latency", np.abs(y - x[:len(y)]).max() == 0.0, f"max error {np.abs(y - x[:len(y)]).max():.2g}")
d = np.abs(aligned(out["brand"], runs[2].latency, len(x)) - aligned(out["b512"], runs[1].latency, len(x))).max()
report("random host block sizes 1-4096 = 512-sample blocks", d < 1e-6, f"max diff {d:.2g}")

# ---------------------------------------------------------------- 2. VOCAL: open on the voice, closed in the pauses
for fs in (44100, 48000, 96000):
    for speed in ("Best", "Fast"):
        x, act = voice(fs, 14.0)
        r = Run("v", fs, sets={"Speed": speed})
        out = run_all([r], {"v": x}, jobname=f"v{fs}{speed}")
        y = aligned(out["v"], r.latency, len(x))[:, 0]; xx = x[:len(y), 0]; a = act[:len(y)]
        # gain per 10 ms block from the output / input energy (the gate is a pure gain)
        B = fs // 100; nb = len(y) // B
        ey = (y[:nb * B] ** 2).reshape(nb, B).sum(1); ex = (xx[:nb * B] ** 2).reshape(nb, B).sum(1) + 1e-30
        g = 10 * np.log10(ey / ex + 1e-30)
        ab = a[:nb * B].reshape(nb, B)
        on = ab.all(1); far = np.convolve(ab.any(1).astype(float), np.ones(31), mode="same") == 0
        skip = np.arange(nb) > int(2.5 * 100)
        open_ok = np.mean(g[on & skip] > -1.0); closed_ok = np.mean(g[far & skip] < -40.0)
        report(f"VOCAL {speed} @ {fs}: open (> -1 dB) during the voice", open_ok > 0.97, f"{100 * open_ok:.1f} % of 10 ms blocks (latency {r.latency})")
        report(f"VOCAL {speed} @ {fs}: closed (< -40 dB) in the pauses", closed_ok > 0.95, f"{100 * closed_ok:.1f} %")
        want_ms = {"Best": 67.0, "Fast": 42.0}[speed]
        report(f"VOCAL {speed} @ {fs}: reported latency {want_ms:.0f} ms (2.0)", abs(r.latency / fs * 1000 - want_ms) < 0.6, f"{r.latency} samples = {r.latency / fs * 1000:.2f} ms")
# 2.0: a real room - no bleed, no hum, strong rumble under 60 Hz (louder than the room tone above it), quiet room tone
fs = 48000
x, act = voice(fs, 14.0, noise_db=-120.0)
rng = np.random.default_rng(31)
rum = sg.sosfilt(sg.butter(2, [4, 60], "bandpass", fs=fs, output="sos"), np.cumsum(rng.standard_normal(len(x))))
rum *= 10 ** (-50 / 20) / np.std(rum)
room = sg.lfilter([1], [1, -0.95], rng.standard_normal(len(x))); room *= 10 ** (-76 / 20) / np.std(room)
xr = x + (rum + room)[:, None]
r = Run("room", fs, sets={"Speed": "Best"})
out = run_all([r], {"room": xr}, jobname="room")
y = aligned(out["room"], r.latency, len(xr))[:, 0]; xx = xr[:len(y), 0]; a = act[:len(y)]
B = fs // 100; nb = len(y) // B
g = 10 * np.log10((y[:nb * B] ** 2).reshape(nb, B).sum(1) / ((xx[:nb * B] ** 2).reshape(nb, B).sum(1) + 1e-30) + 1e-30)
ab = a[:nb * B].reshape(nb, B); on = ab.all(1); far = np.convolve(ab.any(1).astype(float), np.ones(31), mode="same") == 0
skip = np.arange(nb) > int(2.5 * 100)
report("VOCAL BEST, real room (rumble -50 dBFS under 60 Hz, no bleed): closed (< -40 dB) in the pauses", np.mean(g[far & skip] < -40) > 0.95,
       f"{100 * np.mean(g[far & skip] < -40):.1f} %")
report("VOCAL BEST, real room: open (> -1 dB) during the voice", np.mean(g[on & skip] > -1) > 0.97, f"{100 * np.mean(g[on & skip] > -1):.1f} %")
# LIVE (zero latency, level detection)
fs = 48000
t = np.arange(fs) / fs
s = np.where((t > 0.2) & (t < 0.6), 0.1, 0.001) * np.sin(2 * np.pi * 1000 * t)
r = Run("live", fs, ch=1, sets={"Speed": "Live", "Detection": "Level", "Threshold": "-40", "Hold": "0"})
out = run_all([r], {"live": s[:, None]}, jobname="live")
y = out["live"][:, 0]
report("LIVE: zero latency", r.latency == 0, f"{r.latency} samples")
report("LIVE: loud part passes unchanged", np.abs(y[int(0.3 * fs):int(0.5 * fs)] - s[int(0.3 * fs):int(0.5 * fs)]).max() < 1e-6, "")
report("LIVE: quiet part gated to the range (-60 dB)", abs(db(np.std(y[int(0.8 * fs):]) / np.std(s[int(0.8 * fs):])) + 60) < 1.0,
       f"{db(np.std(y[int(0.8 * fs):]) / np.std(s[int(0.8 * fs):])):.1f} dB")
# 2.0: the LEVEL key filters - a 60 Hz tone at -20 dBFS opens with the default key, not through KEY HP 200 Hz
lo = (0.1 * np.sin(2 * np.pi * 60 * t))[:, None]
runs = [Run("k80", fs, ch=1, sets={"Speed": "Live", "Detection": "Level", "Threshold": "-45"}),
        Run("k200", fs, ch=1, sets={"Speed": "Live", "Detection": "Level", "Threshold": "-45", "Key High-Pass": "200"})]
out = run_all(runs, {r.name: lo for r in runs}, jobname="key")
g80 = db(np.std(out["k80"][int(0.3 * fs):, 0]) / np.std(lo[int(0.3 * fs):, 0])); g200 = db(np.std(out["k200"][int(0.3 * fs):, 0]) / np.std(lo[int(0.3 * fs):, 0]))
report("LEVEL key (2.0): 60 Hz at -20 dBFS opens with the default key, stays at the range with KEY HP 200 Hz", g80 > -0.1 and g200 < -59,
       f"{g80:.1f} dB / {g200:.1f} dB")

# ---------------------------------------------------------------- 3. SHAPE: TAIL 50 % halves the decay of an exponential tone
fs = 48000
t = np.arange(int(1.4 * fs)) / fs
hit = np.zeros(int(3.0 * fs))
for t0 in (0.2, 1.6):
    i0 = int(t0 * fs); seg = 0.6 * np.sin(2 * np.pi * 110 * t) * np.exp(-t * 6.0)
    hit[i0:i0 + len(seg)] += seg[:len(hit) - i0]
runs = [Run(f"sh{tl}", fs, ch=1, sets={"Mode": "Shape", "Tail": str(tl), "Threshold": "-70", "Hold": "0", "Release": "500", "Range": "-80"}) for tl in (100, 50)]
out = run_all(runs, {r.name: hit[:, None] for r in runs}, jobname="shape")
def decay_rate(y):   # dB per second of the second hit's envelope between -6 and -30 dB below its peak
    e = np.abs(sg.hilbert(y[int(1.6 * fs):int(2.9 * fs)])); e = sg.lfilter([0.01], [1, -0.99], e)
    edb = 20 * np.log10(e + 1e-12); pk = edb.max(); ip = np.argmax(edb)
    i6 = ip + np.argmax(edb[ip:] < pk - 6); i30 = ip + np.argmax(edb[ip:] < pk - 30)
    return 24.0 / ((i30 - i6) / fs)
r100 = decay_rate(aligned(out["sh100"], runs[0].latency, len(hit))[:, 0]); r50 = decay_rate(aligned(out["sh50"], runs[1].latency, len(hit))[:, 0])
report("SHAPE: TAIL 100 % keeps the natural decay (6/s = 52 dB/s)", abs(r100 - 52.1) < 3.0, f"{r100:.1f} dB/s")
report("SHAPE: TAIL 50 % doubles the decay rate (same shape, half as long)", abs(r50 / r100 - 2.0) < 0.15, f"{r50:.1f} dB/s = {r50 / r100:.2f} x")

# ---------------------------------------------------------------- 4. RHYTHM: free-running 16ths at 120 bpm (no host transport)
fs = 48000
dc = np.full((2 * fs, 1), 0.3)            # a constant input: output / input is exactly the gate's gain
r = Run("rh", fs, ch=1, sets={"Mode": "Rhythm", "Rate": "1/16", "Range": "-80"})
out = run_all([r], {"rh": dc}, jobname="rh")
g = out["rh"][:, 0] / 0.3
on = g > 0.5
edges = np.where(np.diff(on.astype(int)) == 1)[0]
per = np.median(np.diff(edges)) / fs if len(edges) > 2 else 0
report("RHYTHM: the default pattern repeats every 1/8 note at 120 bpm (open 16th, closed 16th)", abs(per - 0.25) < 0.002, f"{per * 1000:.2f} ms")
# 2.0: step levels - the default pattern (steps 1, 3, .. 15 open) plus step 2 open at 50 %: one 16th in 16 sits at
# half gain (the free-running phase is unknown, so the plateaus are measured, not positions)
r = Run("rhl", fs, ch=1, sets={"Mode": "Rhythm", "Rate": "1/16", "Range": "-80", "Step 2": "On", "Step 2 Level": "50"})
out = run_all([r], {"rhl": dc}, jobname="rhl")
g = out["rhl"][:, 0] / 0.3
half = np.abs(g - 0.5) < 0.01; full = np.abs(g - 1.0) < 1e-3
report("RHYTHM step level (2.0): step 2 at 50 % - the half-gain plateau (1/16 of the time) and its level", 0.04 < half.mean() < 0.0625 and 0.4 < full.mean() < 0.5
       and abs(np.median(g[half]) - 0.5) < 2e-3, f"{100 * half.mean():.1f} % of the time at {np.median(g[half]):.4f}, {100 * full.mean():.1f} % at 1")

# ---------------------------------------------------------------- 5. BLEED through the side-chain
fs = 48000
n = 30 * fs
rng = np.random.default_rng(7)
beat = sg.lfilter([1], [1, -0.7], rng.standard_normal((n, 2)), axis=0) * 0.1 * (0.6 + 0.4 * np.sin(2 * np.pi * 2 * np.arange(n) / fs))[:, None]
h = np.zeros(int(0.01 * fs)); h[int(0.0008 * fs)] = 0.03; h[int(0.006 * fs)] = -0.012
leak = sg.lfilter(h, [1], beat[:, 0] + beat[:, 1])
v, act = voice(fs, 30.0, noise_db=-90.0)
mic = f32(v[:, :1] + leak[:, None])
r = Run("bl", fs, ch=1, sets={"Bleed Removal": "On", "Range": "0"})
out = run_all([r], {"bl": mic}, sidechains={"bl": f32(beat)}, jobname="bleed")
y = aligned(out["bl"], r.latency, n)[:, 0]
res = y - v[:len(y), 0]; second = np.arange(len(y)) > len(y) // 2; a = act[:len(y)]
erl_p = 10 * np.log10(np.sum(leak[:len(y)][second & ~a] ** 2) / np.sum(res[second & ~a] ** 2))
erl_v = 10 * np.log10(np.sum(leak[:len(y)][second & a] ** 2) / np.sum(res[second & a] ** 2))
report("BLEED: beat bleed removed in the pauses (second half, gate open)", erl_p > 15, f"{erl_p:.1f} dB")
report("BLEED: beat bleed removed under the voice", erl_v > 10, f"{erl_v:.1f} dB")

# ---------------------------------------------------------------- 6. every factory preset: finite, never louder than the input
names = ["Init", "Rap Lead - Clean Gaps", "Rap Lead - Kill Beat Bleed", "Ad-Libs & Doubles", "Breath Remover", "Natural Singing",
         "Podcast & Voice-Over", "Live Tracking (Zero Latency)", "Tight Kick", "Short Snare", "808 Tamer", "Gated Snare (80s)", "Punchy Toms",
         "Sample Cleanup", "Hats - Super Tight", "Trance Gate 16ths", "Stutter 32nds", "Half-Time Chop", "Swing Gate", "Triplet Pulse",
         "Soft Tremolo 8ths", "Kick Trigger (MIDI)", "Ducking Pump", "Accent Chop", "Live Tracking - Boomy Room"]
fs = 48000
x, act = voice(fs, 5.0)
runs = [Run(f"p{i}", fs, sets={"Program": nm}) for i, nm in enumerate(names)]
out = run_all(runs, {r.name: x for r in runs}, jobname="presets")
bad = [nm for r, nm in zip(runs, names) if not np.isfinite(out[r.name]).all() or np.abs(out[r.name]).max() > np.abs(x).max() * 1.0001]
report(f"all {len(names)} factory presets: finite, never above the input's peak", not bad, ", ".join(bad) if bad else "ok")

fails = sum(1 for _, ok, _ in results if not ok)
print(f"\n{'ALL PASS' if fails == 0 else 'FAILURES'}: {len(results) - fails} of {len(results)} independent checks passed ({time.time() - START:.0f} s)")
sys.exit(fails)
