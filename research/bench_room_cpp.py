"""2.0 room benchmark through the C++ engine: takes recorded the way real takes are - no bleed or bleed, room tone of
any colour, sub-60 Hz rumble, air conditioning, dense rap pauses, a fast compressor on the chain - scored like
bench_vocal_cpp.py against the per-take best classic gate (exhaustive search, cached). "words only": the voice loss
on the labelled words alone (in dense takes the CMU recordings' own sounds just after a word, labelled pause, weigh in
the whole-take figure; the smart gate removes them, a level gate keeps them). The rooms are fixed; the
seeds are not used in training. usage: python3 bench_room_cpp.py <scratch dir with GateRender_no_*> [key=value ...]"""
import os, sys, itertools, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus as C, gate_sim as G
from bench_vocal_cpp import gain_of, TMP, SPK

ROOMS = {
    # a dry booth with a stand on the floor: brownish room tone, rumble over it, rap density, a 4:1 compressor
    "booth": (-70.0, dict(bleed=False, tilt=1.5, hum=False, rumble_db=-56.0, gap=(0.08, 0.5), comp=4.0, comp_thr_db=-8)),
    # a bedroom with air conditioning: bright breathing room tone, normal pauses, no compressor
    "aircon": (-58.0, dict(bleed=False, tilt=0.4, hum=False, hvac_db=3.0, gap=(0.2, 1.2))),
    # headphones loud, everything at once: bleed, rumble, hum, dense, 8:1 compressor
    "loud": (-62.0, dict(bleed=True, tilt=1.0, hum=True, rumble_db=-50.0, gap=(0.08, 0.5), comp=8.0, comp_thr_db=-10)),
    # 2.0: a bare, untreated room like the real take's (RT60 0.33 s in the bass, 0.23 s above), dense rap
    "bare": (-70.0, dict(bleed=False, tilt=1.2, hum=False, rumble_db=-58.0, gap=(0.08, 0.5), rt60=0.25, rt60_lf=1.35, drr_db=9.0)),
}

def classic_frontier(tag, d):
    cc = f"{TMP}/{tag}_classic.npy"
    if os.path.exists(cc):
        return list(np.load(cc, allow_pickle=True))
    flo = 10 * np.log10(np.mean((d["bleed"] + d["noise"]) ** 2))
    out = []
    for thr, look, hold, rel in itertools.product(np.arange(flo + 3, flo + 40, 3), (0, 2, 5, 10), (20, 80, 200), (40, 120, 300)):
        g = G.classic_gain(d["mic"], thr, look_ms=look, hold_ms=hold, release_ms=rel)
        out.append((G.score(g, d), (thr - flo, look, hold, rel)))
    np.save(cc, np.array(out, dtype=object), allow_pickle=True)
    return out

if __name__ == "__main__":
    extra = sys.argv[2:]
    dv, do, bgs = {}, {}, {}
    for room, (ndb, cfg) in ROOMS.items():
        for spk in SPK:
            d = C.take(spk, 10, seed=9000 + len(room), bleed_db=-26.0, noise_db=ndb, breaths=True, room=cfg)
            classic = classic_frontier(f"room_{room}_{spk}", d)
            g, info = gain_of(spk, d["mic"], extra, sys.argv[1])
            s = G.score(g, d)
            n = min(len(g), len(d["voice"])); wm = d["vlab"][:n]; v = d["voice"][:n]
            words = 10 * np.log10(np.sum(((1 - g[:n]) * v)[wm] ** 2) / np.sum(v[wm] ** 2) + 1e-30)
            ok = [c for c in classic if c[0]["bg"] <= s["bg"] + 0.5]
            bv = min(ok, key=lambda c: c[0]["voice"] + 0.5 * c[0]["onset"]) if ok else None
            dv.setdefault(room, []).append(s["voice"] - bv[0]["voice"] if bv is not None else -20)
            do.setdefault(room, []).append(s["onset"] - bv[0]["onset"] if bv is not None else -20)
            bgs.setdefault(room, []).append(s["bg"])
            print(f"{room:7s} {spk}: voice {s['voice']:6.1f} (words only {words:6.1f}) | starts {s['onset']:6.1f} | bg {s['bg']:6.1f} | breaths {s['breath']:6.1f} || classic: "
                  + (f"voice {bv[0]['voice']:6.1f}, starts {bv[0]['onset']:6.1f}" if bv is not None else f"none removes this much (best {min(c[0]['bg'] for c in classic):.1f})"))
    for room in ROOMS:
        print(f"{room:7s} MEAN: voice vs classic {np.mean(dv[room]):+.1f} dB (worst {max(dv[room]):+.1f}) | starts {np.mean(do[room]):+.1f} | bg left {np.mean(bgs[room]):.1f} dB")
    a = sum(dv.values(), []); b = sum(do.values(), []); c = sum(bgs.values(), [])
    print(f"ALL ROOMS: voice vs classic {np.mean(a):+.1f} dB (worst {max(a):+.1f}) | starts {np.mean(b):+.1f} | bg left {np.mean(c):.1f} dB")
