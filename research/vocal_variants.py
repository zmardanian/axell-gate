"""Smart-gate variants vs the classic frontier (cached by bench_vocal.py). Prints, per variant, the mean over takes of
(Axell voice loss - best classic voice loss at >= the same background removal) and the same for word starts:
negative = Axell damages the voice less. usage: python3 vocal_variants.py <VelvetFeatures>"""
import sys, zlib, numpy as np
sys.argv = sys.argv[:2]
import bench_vocal as B, corpus as C, vad_train as V, gate_sim as G

takes = []
for spk in V.SPK:
    for k in range(3):
        rng = np.random.default_rng(zlib.crc32(f"bench/{spk}/{k}".encode()))
        d = C.take(spk, 10, seed=5000 + k, bleed_db=rng.uniform(-36, -20), noise_db=rng.uniform(-68, -54), breaths=True)
        takes.append((spk, k, d, B.features(d["mic"], f"{spk}{k}"), list(np.load(f"{B.TMP}/{spk}{k}_classic.npy", allow_pickle=True))))
models = {te: V.fit(V.X[np.where(V.SS != te)[0][::2]], V.Y[np.where(V.SS != te)[0][::2]]) for te in V.SPK}

VARIANTS = {
    "lag 20, hold 60": dict(loud=True, open_th=0.3, lag_frames=20, look_extra=240, hold_ms=60, follow_ms=0),
    "lag 20, hold 40": dict(loud=True, open_th=0.3, lag_frames=20, look_extra=240, hold_ms=40, follow_ms=0),
    "lag 20, hold 50": dict(loud=True, open_th=0.3, lag_frames=20, look_extra=240, hold_ms=50, follow_ms=0),
    "lag 10, hold 60": dict(loud=True, open_th=0.3, lag_frames=10, look_extra=240, hold_ms=60, follow_ms=0),
    "lag 30, hold 60": dict(loud=True, open_th=0.3, lag_frames=30, look_extra=240, hold_ms=60, follow_ms=0),
    "lag 20, hold 60, open .2": dict(loud=True, open_th=0.2, lag_frames=20, look_extra=240, hold_ms=60, follow_ms=0),
}
for name, kw in VARIANTS.items():
    dv, do, bgs, worst = [], [], [], []
    for spk, k, d, Fk, classic in takes:
        s = G.score(B.smart_gain(d["mic"], Fk, models[spk], **kw), d)
        ok = [c for c in classic if c[0]["bg"] <= s["bg"] + 0.5]
        if not ok:
            dv.append(-20); do.append(-20); bgs.append(s["bg"]); continue
        bv = min(ok, key=lambda c: c[0]["voice"] + 0.5 * c[0]["onset"])
        dv.append(s["voice"] - bv[0]["voice"]); do.append(s["onset"] - bv[0]["onset"]); bgs.append(s["bg"]); worst.append(s["voice"])
    print(f"{name:36s} voice vs classic {np.mean(dv):+6.1f} dB (worst take {max(dv):+5.1f}) | starts {np.mean(do):+6.1f} dB | "
          f"bg left {np.mean(bgs):6.1f} dB | Axell voice loss worst {max(worst) if worst else 0:6.1f} dB")
