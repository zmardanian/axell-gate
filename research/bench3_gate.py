"""Axell Gate 3.0 vs 2.0 on everything held out (usage: python3 bench3_gate.py <scr dir> ...; each scr dir holds
GateRender (the full network) and GateRender_no_<voice> (a held-out fold per CMU ARCTIC voice)):
  wide     2.0's held-out wide benchmark (bench_wide.CONDITIONS, the three ARCTIC voices by their held-out folds)
  setup    the artist's setup with the artist's real beat as bleed (gate_setup.SETUP_CONDITIONS)
  vctk     the wide conditions on VCTK voices no network was trained on: the six of corpus3.TEST_VCTK that 2.0's 24
           VCTK voices did not include (p298 p304 p323 p351 p363 p376), through the full networks
  real     the artist's real takes (no labels; bench_real.metrics): open in the gaps, voice removed, word starts"""
import os, sys, subprocess, numpy as np
os.environ["AXG_VCTK3"] = "1"
os.environ["AXG_VCTK3_TEST"] = "1"
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus as C, bench_wide as BW, gate_setup as GS, bench_real as BR, bench_vocal_cpp as BV
from wav24 import read_wav

UNSEEN = ("p298", "p304", "p323", "p351", "p363", "p376")
TAKES = os.path.expanduser("~/axell-dereverb-data/your-setup/takes.txt")   # personal list, outside the repository

def vctk_set(scr):
    for v in UNSEEN:                                  # the full network stands in for a held-out fold
        link = f"{scr}/GateRender_no_{v}"
        if not os.path.exists(link):
            os.symlink(f"{scr}/GateRender", link)
    allr = []
    for ci, (name, (ndb, bdb, room)) in enumerate(BW.CONDITIONS.items()):
        for v in UNSEEN:
            d = C.take(v, 10, seed=12000 + 17 * ci, bleed_db=bdb, noise_db=ndb, breaths=True, room=room)
            g, _ = BV.gain_of(v, d["mic"], [], scr)
            allr.append(BW.score(g, d))
    m = {k: np.mean([r[k] for r in allr]) for k in allr[0]}
    pm = 100 * sum(r["nmiss"] for r in allr) / sum(r["nv"] for r in allr)
    po = 100 * sum(r["nopen"] for r in allr) / max(1, sum(r["nfar"] for r in allr))
    print(f"  {'ALL':14s} voice {m['voice']:6.1f} | starts {m['starts']:6.1f} | missed {pm:5.2f} % | false open {po:5.2f} % | bg {m['bg']:6.1f}"
          f" | worst voice {max(r['voice'] for r in allr):6.1f}", flush=True)

def real_set(scr):
    rows = []
    for path in [l.strip() for l in open(TAKES) if l.strip()]:
        if not os.path.exists(path):
            continue
        fs, X = read_wav(path); x = X.mean(1) if X.ndim > 1 else X
        if fs != 48000:
            continue
        gap, start, vref = BR.regions(x, fs)
        w = f"{BV.WORK}/{os.getpid()}r"
        BV.wavf(f"{w}.wav", x)
        subprocess.run([f"{scr}/GateRender", f"{w}.wav", f"{w}o.wav", f"gainout={w}g.f32"], check=True, capture_output=True)
        g = np.nan_to_num(np.fromfile(f"{w}g.f32", np.float32).astype(np.float64)[:len(x)], nan=1.0)
        rows.append(BR.metrics(g, x, gap, start))
    a = {k: np.mean([r[k] for r in rows]) for k in rows[0]}
    print(f"  {len(rows)} takes: open in gaps {a['open']:5.1f} % | voice removed {a['voice']:6.1f} dB (worst {max(r['voice'] for r in rows):6.1f}) | "
          f"word starts {a['starts']:6.1f} dB", flush=True)

if __name__ == "__main__":
    os.makedirs(BV.WORK, exist_ok=True)
    for scr in sys.argv[1:]:
        print(f"===== {scr}", flush=True)
        print("== wide (ARCTIC, held-out folds)", flush=True); GS.bench_set(scr, BW.CONDITIONS)
        print("== the artist's setup (real beat as bleed)", flush=True); GS.bench_set(scr, GS.SETUP_CONDITIONS, real=True)
        print("== VCTK voices never trained on (full networks)", flush=True); vctk_set(scr)
        print("== the artist's real takes", flush=True); real_set(scr)
