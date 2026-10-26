"""Gate 3.0: the decision's operating point, calibrated on voices BOTH networks were trained on (VCTK voices in 2.0's
24 and in 3.0's 96; never the benchmark voices or seeds): the SENSITIVITY at which 3.0 closes the pauses as often as
2.0 at its default. usage: python3 calib3_gate.py <2.0 GateRender> <3.0 GateRender> sens...  (prints pooled numbers)"""
import os, sys, numpy as np
os.environ["AXG_VCTK3"] = "1"
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus as C, bench_wide as BW, bench_vocal_cpp as BV

CAL = ("p225", "p226", "p234", "p237", "p245", "p251")   # in both trainings
def run(tool, extra):
    scr = os.path.dirname(tool)
    for v in CAL:
        link = f"{scr}/GateRender_no_{v}"
        if not os.path.exists(link):
            os.symlink(tool, link)
    allr = []
    for ci, (name, (ndb, bdb, room)) in enumerate(BW.CONDITIONS.items()):
        for v in CAL:
            d = C.take(v, 10, seed=22000 + 31 * ci, bleed_db=bdb, noise_db=ndb, breaths=True, room=room)
            g, _ = BV.gain_of(v, d["mic"], extra, scr)
            allr.append(BW.score(g, d))
    po = 100 * sum(r["nopen"] for r in allr) / max(1, sum(r["nfar"] for r in allr))
    pm = 100 * sum(r["nmiss"] for r in allr) / sum(r["nv"] for r in allr)
    return np.mean([r["voice"] for r in allr]), np.mean([r["bg"] for r in allr]), po, pm

if __name__ == "__main__":
    t2, t3 = sys.argv[1], sys.argv[2]
    os.makedirs(BV.WORK, exist_ok=True)
    v, bg, po, pm = run(t2, [])
    print(f"2.0 sens 0: voice {v:6.1f} | bg {bg:6.1f} | false open {po:5.2f} % | missed {pm:4.2f} %", flush=True)
    for s in sys.argv[3:]:
        v, bg, po, pm = run(t3, [f"sens={s}"])
        print(f"3.0 sens {s}: voice {v:6.1f} | bg {bg:6.1f} | false open {po:5.2f} % | missed {pm:4.2f} %", flush=True)
