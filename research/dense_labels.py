"""Labels for a real dense take (the artist's rap) that Gate can learn from. Gate's convention: voice = the dry
voice's activity; a room's tail after a word is background (corpus.take). A level labeller on the recording itself
cannot follow it: in a 0.3 s room, tails sit far above the floor, and in dense rap there is no background mode
(vctk_labels' threshold lands mid-voice). So: Axell De-Reverb (the shipping engine, tools/DRRender) removes the room
first, then level re the take's own floor decides, with a don't-care band in between.
Validated on corpus takes built like the real one (same room, floor and density) against their phone labels:
python3 dense_labels.py --validate"""
import os, sys, subprocess, tempfile, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
DRR = os.path.expanduser("~/axell-dereverb/build/DRRender")
DRW = os.path.expanduser("~/axell-dereverb/Resources/dereverb.bin")
FR = 0.005

def dereverb(x):
    with tempfile.TemporaryDirectory() as d:
        x.astype(np.float32).tofile(f"{d}/i.f32")
        subprocess.run([DRR, DRW, f"{d}/i.f32", f"{d}/o.f32", "--raw"], check=True, capture_output=True)
        return np.fromfile(f"{d}/o.f32", np.float32).astype(np.float64)

def levels(x, fs):
    fr = int(FR * fs); n = len(x) // fr
    return 10 * np.log10(np.mean(x[:n * fr].reshape(n, fr) ** 2, 1) + 1e-14)

def runs(m):
    d = np.diff(np.concatenate([[0], m.astype(int), [0]]))
    return list(zip(np.where(d == 1)[0], np.where(d == -1)[0]))

def label(x, fs, tv=12.0, tb=3.0, th=8.0, y=None):
    """per 5 ms frame: 1 voice, 0 background, 3 don't care. tv / tb: dB over the floor of the de-reverbed take"""
    y = dereverb(x) if y is None else y
    lev = levels(y, fs)
    live = lev > -120
    floor = np.percentile(lev[live], 3)
    on = lev > floor + tv
    for a, b in runs(on):                       # islands under 30 ms: clicks
        if b - a < 6:
            on[a:b] = False
    # hysteresis: a word goes on while the de-reverbed level stays th dB over the floor (quiet endings, soft consonants)
    hold = lev > floor + th
    for a, b in runs(on):
        e = b
        while e < len(on) and hold[e] and not on[e]:
            e += 1
        on[b:e] = True
        s0 = a
        while s0 > 0 and hold[s0 - 1] and not on[s0 - 1]:
            s0 -= 1
        on[s0:a] = True
    out = np.where(on, 1, np.where(lev < floor + tb, 0, 3))
    for a, b in runs(~on):                      # gaps under 150 ms: don't care (G19's convention)
        if a > 0 and b < len(on) and b - a < 30:
            out[a:b] = 3
    for a, b in runs(on):                       # 20 ms before an onset: don't care (soft onsets)
        out[max(0, a - 4):a] = np.where(out[max(0, a - 4):a] == 0, 3, out[max(0, a - 4):a])
    out[~live] = 0                              # digital silence is background
    return out

def truth_frames(vlab, n, fs):
    fr = int(FR * fs)
    return vlab[:n * fr].reshape(n, fr).mean(1) > 0.5

def synth(spk, seed, fs=48000):
    import corpus as C
    room = dict(bleed=False, tilt=1.0, hum=False, rumble_db=None, hvac_db=0, gap=(0.02, 0.25), comp=None,
                rt60=0.31, rt60_lf=1.3, drr_db=8.0)
    d = C.take(spk, 8, seed=seed, bleed_db=-60, noise_db=-18 - 21, breaths=True, room=room)
    return d["mic"], d["vlab"]

def score(lab, truth):
    m = lab != 3
    v, b = truth[m], lab[m]
    return dict(miss=100 * np.mean(b[v] == 0), false=100 * np.mean(b[~v] == 1), dontcare=100 * np.mean(~m),
                agree=100 * np.mean((b == 1) == v))

if __name__ == "__main__" and "--validate" in sys.argv:
    import corpus as C
    fit_spk = ["slt", "bdl", "jmk"]; val_spk = C.vctk_speakers()[:8]
    cache = {}
    def data(spk, k):
        if (spk, k) not in cache:
            x, vl = synth(spk, 500 + k); cache[(spk, k)] = (x, vl, dereverb(x))
        return cache[(spk, k)]
    def run(spks, tv, tb, th):
        rows = []
        for s in spks:
            for k in range(2):
                x, vl, y = data(s, k)
                lab = label(x, 48000, tv, tb, th, y=y); t = truth_frames(vl, len(lab), 48000)
                rows.append(score(lab, t))
        return {k: np.mean([r[k] for r in rows]) for k in rows[0]}
    best = None
    for tv in (9, 12, 15):
      for tb in (2, 3, 4):
        for th in (4, 6, 8):
            if not tb < th < tv: continue
            r = run(fit_spk, tv, tb, th)
            print(f"fit  tv {tv:2d} tb {tb} th {th}: agree {r['agree']:.1f} %  voice->background {r['miss']:.1f} %  background->voice {r['false']:.1f} %  don't care {r['dontcare']:.1f} %")
            cost = r["miss"] * 2 + r["false"] + 0.1 * r["dontcare"]   # missing voice is the worse error (G19)
            if best is None or cost < best[0]: best = (cost, tv, tb, th)
    _, tv, tb, th = best
    r = run(val_spk, tv, tb, th)
    print(f"VALIDATION on {len(val_spk)} VCTK voices never used for the fit (tv {tv}, tb {tb}, th {th}): agree {r['agree']:.1f} %, "
          f"voice->background {r['miss']:.1f} %, background->voice {r['false']:.1f} %, don't care {r['dontcare']:.1f} %")
    raw = []
    for s in val_spk:
        for k in range(2):
            x, vl, y = data(s, k)
            segs = C.vctk_labels(x, 48000); fr = int(0.005 * 48000); n = len(x) // fr
            v = np.zeros(n, bool); g = np.zeros(n, bool)
            for a, b, p in segs:
                (v if p in ("v",) else g if p == "gap" else np.zeros(n, bool))[int(a / 0.005):int(b / 0.005)] = True
            lab = np.where(g, 3, np.where(v, 1, 0)); raw.append(score(lab, truth_frames(vl, n, 48000)))
    print("for comparison, Gate's VCTK labeller on the same takes: " + ", ".join(f"{k} {np.mean([q[k] for q in raw]):.1f}" for k in raw[0]))
