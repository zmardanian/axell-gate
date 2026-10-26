"""Voice-activity training / test data: vocal takes (corpus.take) with random bleed, noise, breaths and recording
chains, run through Axell Velvet's shipping C++ feature extractor (VelvetFeatures: 55 columns per 2 ms frame) at
44.1 / 48 / 96 kHz. Labels per frame centre: voice (any non-pause phone), breath, else background.
usage: python3 vad_data.py <VelvetFeatures binary>  -> ~/axell-gate-data/vad/{feat,lab,meta}.npz"""
import os, sys, zlib, subprocess, numpy as np
from concurrent.futures import ProcessPoolExecutor
from scipy import signal as sg
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus as C

OUT = os.path.expanduser(os.environ.get("AXG_VAD_OUT", "~/axell-gate-data/vad"))
SPK = ("slt", "bdl", "jmk")
NC = 55

N_TAKES = 36   # per CMU ARCTIC voice; 1.0 had 18 takes, all in the 1.0 room
N_VCTK = int(os.environ.get("AXG_N_VCTK", 8))   # per VCTK voice (2.0: 8 x 24 voices; 3.0: 4 x 96 MFA-labelled voices)

def jobs_list():
    return [(s, k) for s in SPK for k in range(N_TAKES)] + [(s, k) for s in C.vctk_speakers() for k in range(N_VCTK)]

def room_of(k, rng):
    """2.0: one take in three is the 1.0 room (bleed, pink noise and hum); the others are real rooms: often no
    bleed, room tone white to brown, rarely hum, rumble (as loud as 22 dB under the voice), breathing air
    conditioning, dense rap pauses, a fast compressor on the chain, untreated-room reverb"""
    if (k // 3) % 3 == 0:   # (k % 3 sets the sample rate)
        return None
    return dict(bleed=rng.random() < 0.4, tilt=rng.uniform(0.0, 2.0), hum=rng.random() < 0.2,
                rumble_db=rng.uniform(-80, -40) if rng.random() < 0.6 else None,
                hvac_db=rng.uniform(0, 4) if rng.random() < 0.4 else 0,
                gap=(0.08, 0.5) if rng.random() < 0.5 else (0.2, 1.4),
                comp=rng.uniform(3, 12) if rng.random() < 0.4 else None, comp_thr_db=rng.uniform(-14, -4),
                # untreated rooms (2.0, after a real take from a bare room: RT60 0.33 s in the bass, 0.23 s above)
                rt60=rng.uniform(0.15, 0.7) if rng.random() < 0.6 else None, rt60_lf=rng.uniform(1.0, 1.8), drr_db=rng.uniform(4, 16))

def job(a):
    spk, k = a
    rng = np.random.default_rng(zlib.crc32(f"vad/{spk}/{k}".encode()))
    room = room_of(k, rng)
    ndb = rng.uniform(-72, -50) if room is None else rng.uniform(-95, -50)   # real rooms: down to 77 dB under the voice
    d = C.take(spk, 8, seed=1000 + k, bleed_db=rng.uniform(-40, -18), noise_db=ndb, breaths=rng.random() < 0.8, room=room)
    mic = d["mic"]
    # a random recording chain on top (mic colour, level)
    sys.path.insert(0, os.path.expanduser("~/axell-velvet/research"))
    import train_cpp as T
    if rng.random() < 0.5:
        b, a_ = T.peaking(C.FS, rng.uniform(3000, 12000), rng.uniform(0.7, 2), rng.uniform(0, 6)); mic = sg.lfilter(b, a_, mic)
    g = 10 ** (rng.uniform(-24, 6) / 20)
    mic = mic * g
    fs = (44100, 48000, 96000)[k % 3]
    if fs != C.FS:
        mic = sg.resample_poly(mic, {44100: 147, 96000: 2}[fs], {44100: 160, 96000: 1}[fs])
    p = f"{OUT}/{spk}_{k}.f32"
    mic.astype(np.float32).tofile(p)
    # labels on the 48 kHz timeline (seconds)
    return f"{p} {fs} {OUT}/{spk}_{k}.feat\n", (spk, k, fs, d["vlab"], d["blab"], d["xlab"])

def labels_at(t, vlab, blab, xlab=None):
    """0 background, 1 voice, 2 breath, 3 don't care (VCTK's filled short gaps: left out of training)"""
    i = np.clip((t * C.FS).astype(int), 0, len(vlab) - 1)
    y = np.where(vlab[i], 1, np.where(blab[i], 2, 0))
    return y if xlab is None else np.where(xlab[i], 3, y)

if __name__ == "__main__":
    tool = sys.argv[1]
    os.makedirs(OUT, exist_ok=True)
    jobs = jobs_list()
    with ProcessPoolExecutor() as ex:
        res = list(ex.map(job, jobs))
    mans = []
    for i in range(12):
        mp = f"{OUT}/manifest{i}.txt"; open(mp, "w").writelines([r[0] for r in res][i::12]); mans.append(mp)
    procs = [subprocess.Popen([tool, m], stdout=subprocess.DEVNULL) for m in mans]
    assert all(p.wait() == 0 for p in procs)
    F, Lb, S, K = [], [], [], []
    for line, (spk, k, fs, vlab, blab, xlab) in res:
        os.remove(line.split()[0])
        d = np.fromfile(f"{OUT}/{spk}_{k}.feat", np.float32).reshape(-1, NC)
        os.remove(f"{OUT}/{spk}_{k}.feat")
        F.append(d); Lb.append(labels_at(d[:, 0].astype(float), vlab, blab, xlab)); S.append(np.full(len(d), spk)); K.append(np.full(len(d), k))
    np.savez(f"{OUT}/vad.npz", F=np.concatenate(F), L=np.concatenate(Lb), S=np.concatenate(S), K=np.concatenate(K))
    L = np.concatenate(Lb)
    print(f"{len(L)} frames: voice {100 * (L == 1).mean():.1f} %, breath {100 * (L == 2).mean():.1f} %, background {100 * (L == 0).mean():.1f} %")
