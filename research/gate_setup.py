"""Axell Gate 2.1: the voice-activity network also trained on the artist's setup (untreated room -> LCT 440 PURE ->
Volt 176), measured from the artist's dry takes (axell-dereverb/research/measure_setup.py):
  room   mids/highs decay in 0.13-0.24 s, the bass (< 300 Hz) 1.3-5x longer (0.26-0.73 s over the takes);
  noise  1/f^1.3-1.7 (the measured floor falls ~4.6 dB/octave), 22-40 dB under the voice;
  76     the Volt's 1176-style compressor on half the takes (ratio 4-8);
  bleed  the artist's real beat (a with-beat take minus its acapella) leaking from closed headphones, on 60 %;
  delivery: dense rap gaps on 60 % of the takes.
Voices and labels are the corpus's (CMU ARCTIC phone labels; VCTK's calibrated labeller), so the labels are true.
The original training data (train_gate.py, all.npz) is kept; the setup takes are added (~26 % of the takes).
usage: python3 gate_setup.py data | train | build | bench"""
import os, sys, zlib, subprocess, numpy as np
from concurrent.futures import ProcessPoolExecutor
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus as C, vad_data as VD

H = os.path.expanduser("~")
OUT = f"{H}/axell-gate-data/gatefeat_setup"
BEAT = f"{H}/axell-gate-data/beat-pair/beat.npy"
# the artist's beat: an acapella and the same take with the beat (a personal pair, outside the repository: two paths in
# ~/axell-gate-data/beat-pair/pair.txt, acapella first); the beat is their difference
PAIR = f"{H}/axell-gate-data/beat-pair/pair.txt"
N_ARCTIC, N_VCTK = 12, 3
K, NC = 27, 1 + 27 + 6
SPK = ("slt", "bdl", "jmk")

def make_beat():
    import soundfile as sf
    pa, pb = [l.strip() for l in open(PAIR) if l.strip()][:2]
    a, fs = sf.read(pa); b, _ = sf.read(pb)
    np.save(BEAT, (b - a).astype(np.float32))

def real_beat(secs, seed=1):
    bt = np.load(BEAT, mmap_mode="r"); n = int(secs * C.FS)
    rng = np.random.default_rng(seed); out = np.zeros((n, 2)); pos = 0
    while pos < n:                                   # loop the 46 s beat from a random point
        s0 = int(rng.integers(len(bt) // 2)); seg = np.asarray(bt[s0:s0 + n - pos], np.float64)
        out[pos:pos + len(seg)] = seg; pos += len(seg)
    out *= 10 ** (-8 / 20) / (np.max(np.abs(out)) + 1e-12)
    return out, None

def setup_room(rng):
    return dict(bleed=rng.random() < 0.6, tilt=rng.uniform(1.3, 1.7), hum=False,
                rumble_db=rng.uniform(-75, -50) if rng.random() < 0.4 else None, hvac_db=rng.uniform(0, 2) if rng.random() < 0.3 else 0,
                gap=(0.0, 0.15) if rng.random() < 0.6 else (0.15, 0.8),
                comp=rng.uniform(4, 8) if rng.random() < 0.5 else None, comp_thr_db=rng.uniform(-14, -4),
                rt60=rng.uniform(0.13, 0.24), rt60_lf=rng.uniform(1.3, 5.0), drr_db=rng.uniform(4, 14))

def jobs():
    return [(s, k) for s in SPK for k in range(N_ARCTIC)] + [(s, k) for s in C.vctk_speakers() for k in range(N_VCTK)]

def job(a):
    spk, k = a
    C.beat = real_beat                               # this worker's takes: the artist's beat in the headphones
    rng = np.random.default_rng(zlib.crc32(f"setup/{spk}/{k}".encode()))
    d = C.take(spk, 8, seed=50000 + k, bleed_db=rng.uniform(-40, -22), noise_db=-18 - rng.uniform(22, 40), breaths=rng.random() < 0.8, room=setup_room(rng))
    mic = d["mic"] * 10 ** (rng.uniform(-18, 6) / 20)
    fs = (44100, 48000, 96000)[k % 3]
    if fs != C.FS:
        from scipy import signal as sg
        mic = sg.resample_poly(mic, {44100: 147, 96000: 2}[fs], {44100: 160, 96000: 1}[fs])
    p = f"{OUT}/{spk}_{k}.f32"; mic.astype(np.float32).tofile(p)
    return f"{p} {fs} {OUT}/{spk}_{k}.feat\n", (spk, k, fs, d["vlab"], d["blab"], d["xlab"])

def data():
    os.makedirs(OUT, exist_ok=True)
    if not os.path.exists(BEAT):
        make_beat()
    with ProcessPoolExecutor(4) as ex:
        res = list(ex.map(job, jobs()))
    mans = []
    for i in range(4):
        mp = f"{OUT}/manifest{i}.txt"; open(mp, "w").writelines([r[0] for r in res][i::4]); mans.append(mp)
    tool = f"{H}/axell-gate/build/GateFeatures"
    assert all(p.wait() == 0 for p in [subprocess.Popen([tool, m], stdout=subprocess.DEVNULL) for m in mans])
    X, Y, S = [], [], []
    for line, (spk, k, fs, vlab, blab, xlab) in res:
        os.remove(line.split()[0])
        d = np.fromfile(f"{OUT}/{spk}_{k}.feat", np.float32).reshape(-1, NC).astype(np.float64); os.remove(f"{OUT}/{spk}_{k}.feat")
        X.append(d); Y.append(VD.labels_at(d[:, 0], vlab, blab, xlab)); S.append(np.full(len(d), spk))
    np.savez(f"{OUT}/setup.npz", D=np.concatenate(X), Y=np.concatenate(Y), S=np.concatenate(S))
    print(len(res), "setup takes,", sum(len(x) for x in X), "frames")

def train():
    import train_gate as TG
    z, s = np.load(f"{H}/axell-gate-data/gatefeat/all.npz"), np.load(f"{OUT}/setup.npz")
    D = np.concatenate([z["D"], s["D"]]); Y = np.concatenate([z["Y"], s["Y"]]); S = np.concatenate([z["S"], s["S"]])
    keep = Y != 3; D, Y, S = D[keep], Y[keep], S[keep]
    X = D[:, 1:1 + K]
    print(f"{len(X)} frames ({len(s['D'])} from the setup takes): voice {100 * (Y == 1).mean():.1f} %, breath {100 * (Y == 2).mean():.1f} %", flush=True)
    md = f"{H}/axell-gate-data/models_setup"; os.makedirs(md, exist_ok=True)
    for te in SPK:
        tr = np.where(S != te)[0][::2]
        m = TG.fit(X[tr], Y[tr])
        TG.write_header(f"{md}/no_{te}.h", m, len(tr), f"held-out fold: without {te}; with the artist's setup", float((Y[tr] == 1).mean()))
        print("fold", te, "written", flush=True)
    m = TG.fit(X[::2], Y[::2], epochs=14)
    TG.write_header(f"{md}/VoiceModel.h", m, len(X) // 2, "all voices; with the artist's setup (2.1)", float((Y[::2] == 1).mean()))
    print("wrote", md)

def build():
    for name, md in (("base", f"{H}/axell-gate-data/models"), ("setup", f"{H}/axell-gate-data/models_setup")):
        scr = f"{H}/axell-gate-data/scr_{name}"; os.makedirs(scr, exist_ok=True)
        for spk in SPK:
            cmd = ["c++", f"-I{H}/axell-gate/Source", f"-I{H}/axell-core/include", "-O3", "-DNDEBUG", "-std=gnu++17",
                   f'-DAXG_VOICE_MODEL_HEADER="{md}/no_{spk}.h"', f"{H}/axell-gate/tools/GateRender.cpp", "-o", f"{scr}/GateRender_no_{spk}"]
            subprocess.run(["distrobox", "enter", "juce-build", "--"] + cmd, check=True)
        print("built", scr)

SETUP_CONDITIONS = {
    "you: dry": (-18 - 30, -60.0, dict(bleed=False, tilt=1.5, hum=False, gap=(0.0, 0.15), rt60=0.17, rt60_lf=1.7, drr_db=9.0)),
    "you: +76": (-18 - 30, -60.0, dict(bleed=False, tilt=1.5, hum=False, gap=(0.0, 0.15), rt60=0.19, rt60_lf=3.8, drr_db=8.0, comp=6.0, comp_thr_db=-8)),
    "you: +bleed": (-18 - 30, -28.0, dict(bleed=True, tilt=1.5, hum=False, gap=(0.05, 0.6), rt60=0.16, rt60_lf=2.0, drr_db=10.0)),
    "you: loud fl.": (-18 - 23, -60.0, dict(bleed=False, tilt=1.6, hum=False, gap=(0.1, 0.6), rt60=0.21, rt60_lf=3.0, drr_db=6.0, comp=4.0)),
}

def bench_set(scr, conditions, real=False):
    """bench_wide.py's loop and scores (held-out voices, never-trained seeds) on a set of conditions"""
    import bench_wide as BW
    from bench_vocal_cpp import gain_of
    saved = C.beat
    if real:
        C.beat = real_beat
    allr = []
    for ci, (name, (ndb, bdb, room)) in enumerate(conditions.items()):
        rs = []
        for spk in SPK:
            d = C.take(spk, 10, seed=12000 + 17 * ci, bleed_db=bdb, noise_db=ndb, breaths=True, room=room)
            g, _ = gain_of(spk, d["mic"], [], scr)
            rs.append(BW.score(g, d))
        allr += rs
        m = {k: np.mean([r[k] for r in rs]) for k in rs[0]}
        pm = 100 * sum(r["nmiss"] for r in rs) / sum(r["nv"] for r in rs); nf = sum(r["nfar"] for r in rs)
        po = 100 * sum(r["nopen"] for r in rs) / max(1, nf)
        print(f"  {name:14s} voice {m['voice']:6.1f} | starts {m['starts']:6.1f} | missed {pm:5.2f} % | false open {po:5.2f} % | bg {m['bg']:6.1f}", flush=True)
    C.beat = saved
    m = {k: np.mean([r[k] for r in allr]) for k in allr[0]}
    pm = 100 * sum(r["nmiss"] for r in allr) / sum(r["nv"] for r in allr)
    po = 100 * sum(r["nopen"] for r in allr) / max(1, sum(r["nfar"] for r in allr))
    print(f"  {'ALL':14s} voice {m['voice']:6.1f} | starts {m['starts']:6.1f} | missed {pm:5.2f} % | false open {po:5.2f} % | bg {m['bg']:6.1f}"
          f" | worst voice {max(r['voice'] for r in allr):6.1f}", flush=True)

def bench():
    import bench_wide as BW
    names = sys.argv[2:] or ["base", "setup"]
    for name in names:
        scr = f"{H}/axell-gate-data/scr_{name}"
        print(f"== {name}: the artist's setup (the real beat as bleed)", flush=True)
        bench_set(scr, SETUP_CONDITIONS, real=True)
        print(f"== {name}: the 2.0 wide benchmark (unchanged conditions)", flush=True)
        bench_set(scr, BW.CONDITIONS)

if __name__ == "__main__":
    dict(data=data, train=train, build=build, bench=bench)[sys.argv[1]]()
