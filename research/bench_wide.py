"""2.0 wide benchmark: 8 recording conditions x 3 voices (held-out models), 24 takes never used in training or tuning,
scored on absolute, time-based measures (no classic-gate frontier; robust to single events):
  voice    energy of the voice removed, re the voice (dB)
  starts   the same over the first 40 ms of every word (dB)
  missed   time of voice with the gate below -6 dB (%)
  falseop  time of the pauses (>= 125 ms from voice, breaths excluded) with the gate above -20 dB (%), pooled over
           the takes (dense-rap takes have little such time; a per-take mean would let a few samples dominate)
  bg       background energy left in those pauses (dB)
usage: python3 bench_wide.py <dir with GateRender_no_*> [key=value ...]"""
import os, sys, numpy as np
from scipy import signal as sg
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus as C, gate_sim as G
from bench_vocal_cpp import gain_of, SPK

CONDITIONS = {
    "studio+bleed": (-62.0, -26.0, None),
    "booth+rumble": (-72.0, -26.0, dict(bleed=False, tilt=1.6, hum=False, rumble_db=-54.0, gap=(0.08, 0.5), comp=4.0)),
    "aircon": (-56.0, -26.0, dict(bleed=False, tilt=0.3, hum=False, hvac_db=4.0, gap=(0.2, 1.2))),
    "bleed+comp": (-64.0, -24.0, dict(bleed=True, tilt=1.0, hum=True, rumble_db=-56.0, gap=(0.08, 0.5), comp=8.0, comp_thr_db=-10)),
    "bare 0.25 s": (-70.0, -26.0, dict(bleed=False, tilt=1.2, hum=False, rumble_db=-58.0, gap=(0.08, 0.5), rt60=0.25, rt60_lf=1.35, drr_db=9.0)),
    "bare 0.5 s": (-66.0, -26.0, dict(bleed=False, tilt=1.0, hum=False, gap=(0.2, 1.0), rt60=0.5, rt60_lf=1.5, drr_db=6.0)),
    "bare+bleed": (-68.0, -28.0, dict(bleed=True, tilt=1.2, hum=False, gap=(0.08, 0.6), rt60=0.35, rt60_lf=1.4, drr_db=8.0, comp=4.0)),
    "quiet room": (-84.0, -26.0, dict(bleed=False, tilt=0.8, hum=False, gap=(0.1, 0.8))),
}

def score(g, d):
    v, vl, n = d["voice"], d["vlab"], min(len(g), len(d["voice"]))
    g, v, vl = g[:n], v[:n], vl[:n]
    i = (d["bleed"] + d["noise"])[:n]
    on = np.zeros(n, bool)
    for a, e in G.segments(vl):
        on[a:min(e, a + int(0.04 * C.FS))] = True
    far = (sg.convolve(vl.astype(float), np.ones(int(0.25 * C.FS)), mode="same") == 0) & ~d["blab"][:n]
    db = lambda a, b: 10 * np.log10((a + 1e-30) / (b + 1e-30))
    return dict(voice=db(np.sum(((1 - g) * v)[vl] ** 2), np.sum(v[vl] ** 2)), starts=max(-80.0, db(np.sum(((1 - g) * v)[on] ** 2), np.sum(v[on] ** 2))),
                missed=100 * np.mean(g[vl] < 0.5), falseop=100 * np.mean(g[far] > 0.1) if far.any() else 0.0,
                bg=db(np.sum((g * i)[far] ** 2), np.sum(i[far] ** 2)),
                nv=int(vl.sum()), nmiss=int(np.sum(g[vl] < 0.5)), nfar=int(far.sum()), nopen=int(np.sum(g[far] > 0.1)))

if __name__ == "__main__":
    scr, extra = sys.argv[1], sys.argv[2:]
    allr = []
    for ci, (name, (ndb, bdb, room)) in enumerate(CONDITIONS.items()):
        rs = []
        for spk in SPK:
            d = C.take(spk, 10, seed=12000 + 17 * ci, bleed_db=bdb, noise_db=ndb, breaths=True, room=room)
            g, _ = gain_of(spk, d["mic"], extra, scr)
            rs.append(score(g, d))
        m = {k: np.mean([r[k] for r in rs]) for k in rs[0]}
        allr += rs
        pm = 100 * sum(r["nmiss"] for r in rs) / sum(r["nv"] for r in rs); nf = sum(r["nfar"] for r in rs)
        po = 100 * sum(r["nopen"] for r in rs) / max(1, nf)
        print(f"{name:13s} voice {m['voice']:6.1f} | starts {m['starts']:6.1f} | missed {pm:5.2f} % | false open {po:5.2f} % of {nf / C.FS:4.1f} s | bg {m['bg']:6.1f}", flush=True)
    m = {k: np.mean([r[k] for r in allr]) for k in allr[0]}
    pm = 100 * sum(r["nmiss"] for r in allr) / sum(r["nv"] for r in allr)
    po = 100 * sum(r["nopen"] for r in allr) / max(1, sum(r["nfar"] for r in allr))
    big = [r for r in allr if r["nfar"] > 0.5 * C.FS] or allr   # takes with 0.5 s or more of pauses
    print(f"{'ALL':13s} voice {m['voice']:6.1f} | starts {m['starts']:6.1f} | missed {pm:5.2f} % | false open {po:5.2f} % (pooled) | bg {m['bg']:6.1f}")
    print(f"{'ALL (worst)':13s} voice {max(r['voice'] for r in allr):6.1f} | starts {max(r['starts'] for r in allr):6.1f} | missed {max(r['missed'] for r in allr):5.2f} % | "
          f"false open {max(r['falseop'] for r in big):5.2f} % (takes with 0.5 s+ of pauses)")
