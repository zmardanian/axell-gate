"""Vocal benchmark through the C++ engine (GateRender, held-out voice model per speaker): the same takes, scoring and
classic-gate frontier as bench_vocal.py. usage: python3 bench_vocal_cpp.py <scratch dir with GateRender_no_*> [key=value ...]"""
import os, sys, zlib, subprocess, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus as C, gate_sim as G
SCR = sys.argv[1] if len(sys.argv) > 1 else "."; EXTRA = sys.argv[2:]
TMP = os.path.expanduser("~/axell-gate-data/bench")                      # cached classic frontiers
WORK = os.environ.get("AXG_BENCH_TMP", TMP)                               # scratch renders (per process)
SPK = ("slt", "bdl", "jmk")

def wavf(p, x):
    import struct
    d = np.asarray(x, "<f4").tobytes()
    with open(p, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(d)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 3, 1, C.FS, C.FS * 4, 4, 32) + b"data" + struct.pack("<I", len(d)) + d)

def gain_of(spk, mic, extra, scr=None):
    os.makedirs(WORK, exist_ok=True); w = f"{WORK}/{os.getpid()}"
    wavf(f"{w}m.wav", mic)
    r = subprocess.run([f"{scr or SCR}/GateRender_no_{spk}", f"{w}m.wav", f"{w}o.wav", f"gainout={w}g.f32"] + extra, capture_output=True, text=True, check=True)
    g = np.fromfile(f"{w}g.f32", np.float32).astype(np.float64)
    bad = ~np.isfinite(g)
    if bad.any():
        g[bad] = np.interp(np.where(bad)[0], np.where(~bad)[0], g[~bad])
    return g, r.stdout.strip()

if __name__ == "__main__":
    dv, do, bgs, br = [], [], [], []
    for spk in SPK:
        for k in range(3):
            rng = np.random.default_rng(zlib.crc32(f"bench/{spk}/{k}".encode()))
            d = C.take(spk, 10, seed=5000 + k, bleed_db=rng.uniform(-36, -20), noise_db=rng.uniform(-68, -54), breaths=True)
            classic = list(np.load(f"{TMP}/{spk}{k}_classic.npy", allow_pickle=True))
            g, info = gain_of(spk, d["mic"], EXTRA)
            s = G.score(g, d)
            ok = [c for c in classic if c[0]["bg"] <= s["bg"] + 0.5]
            bv = min(ok, key=lambda c: c[0]["voice"] + 0.5 * c[0]["onset"]) if ok else None
            dv.append(s["voice"] - bv[0]["voice"] if bv is not None else -20); do.append(s["onset"] - bv[0]["onset"] if bv is not None else -20); bgs.append(s["bg"]); br.append(s["breath"])
            print(f"{spk}{k}: voice {s['voice']:6.1f} | starts {s['onset']:6.1f} | bg {s['bg']:6.1f} | breaths {s['breath']:6.1f} || classic at this removal: "
                  + (f"voice {bv[0]['voice']:6.1f}, starts {bv[0]['onset']:6.1f}" if bv is not None else "none") + f"   [{info}]")
    print(f"MEAN: voice vs classic {np.mean(dv):+.1f} dB (worst take {max(dv):+.1f}) | starts {np.mean(do):+.1f} dB | bg left {np.mean(bgs):.1f} dB | breaths {np.mean(br):.1f} dB")
