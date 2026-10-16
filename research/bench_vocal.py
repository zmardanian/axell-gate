"""Vocal gate benchmark: the best-tuned classic level gate (exhaustive parameter search per take, i.e. a perfect
engineer) against the Axell smart gate (one fixed setting), on held-out voices with known components.
usage: python3 bench_vocal.py <VelvetFeatures binary>"""
import os, sys, subprocess, zlib, itertools, numpy as np
from scipy import signal as sg
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus as C
import gate_sim as G
import vad_train as V

TOOL = sys.argv[1]
TMP = os.path.expanduser("~/axell-gate-data/bench")
os.makedirs(TMP, exist_ok=True)

def features(mic, tag):
    cache = f"{TMP}/{tag}_feat.npy"
    if os.path.exists(cache):
        return np.load(cache)
    p = f"{TMP}/{tag}.f32"; mic.astype(np.float32).tofile(p)
    open(f"{TMP}/{tag}.txt", "w").write(f"{p} {C.FS} {TMP}/{tag}.feat\n")
    subprocess.run([TOOL, f"{TMP}/{tag}.txt"], check=True, stdout=subprocess.DEVNULL)
    d = np.fromfile(f"{TMP}/{tag}.feat", np.float32).reshape(-1, 55).astype(np.float64)
    for e in (".f32", ".txt", ".feat"):
        os.remove(f"{TMP}/{tag}{e}")
    np.save(cache, d)
    return d

def hmm_fixed_lag(pv, lag, stay_v=0.998, stay_b=0.995, prior_v=0.73):
    """two-state HMM (background / voice), emissions = the network's posterior over its training prior (scaled
    likelihoods); P(voice at t | evidence up to t + lag) by a forward pass and a lag-frame backward pass"""
    n = len(pv)
    T = np.array([[stay_b, 1 - stay_b], [1 - stay_v, stay_v]])
    lik = np.column_stack([(1 - pv) / (1 - prior_v), pv / prior_v]) + 1e-9
    alpha = np.zeros((n, 2)); a = np.array([0.5, 0.5])
    for t in range(n):
        a = (a @ T) * lik[t]; a /= a.sum(); alpha[t] = a
    post = np.zeros(n)
    for t in range(n):
        e = min(n - 1, t + lag)
        b = np.ones(2)
        for u in range(e, t, -1):
            b = T @ (lik[u] * b); b /= b.sum()
        q = alpha[t] * b; post[t] = q[1] / q.sum()
    return post

def loud_evidence(Fk, pv, below_voice=18.0, above_floor=12.0):
    """P(voice) from level alone, where level is unambiguous: within `below_voice` dB of the voice's own running peak
    level (learned from frames the network is sure about) and `above_floor` dB above the tracked background floor"""
    lev = Fk[:, 51]; flo = V.floor_track(lev)
    ref = np.empty_like(lev); r = None
    for t in range(len(lev)):
        if pv[t] > 0.8:
            r = lev[t] if r is None else max(lev[t], r - 0.002)       # peak, falling 1 dB/s
        elif r is not None:
            r -= 0.002
        ref[t] = r if r is not None else 999.0
    sig = lambda z: 1 / (1 + np.exp(-z))
    return sig((lev - (ref - below_voice)) / 1.5) * sig((lev - flo - above_floor) / 1.5)

def smart_gain(mic, Fk, model, lag_frames=10, keep_breath=False, rng_db=-60.0, release_ms=80.0, open_th=0.5, loud=False,
               stay_v=0.998, look_extra=96, hold_ms=0.0, wiener_k=4.0, follow_ms=400.0):
    X = V.inputs(Fk)
    p = V.prob(X, model)
    pv = p[:, 1] + (p[:, 2] if keep_breath else 0.0)
    if loud:
        pv = np.maximum(pv, loud_evidence(Fk, p[:, 1]))
    post = hmm_fixed_lag(np.clip(pv, 1e-4, 1 - 1e-4), lag_frames, stay_v=stay_v)
    op = post > open_th
    if hold_ms > 0:   # stay open hold_ms after the last voice frame
        h = int(hold_ms / 2)
        op = np.convolve(op.astype(float), np.ones(h + 1), mode="full")[:len(op)] > 0
    # frame centres: VelvetFeatures reports the centre time of each frame; align decisions to samples
    t0 = int(round(Fk[0, 0] * C.FS))
    fr = 96
    opn = np.concatenate([np.zeros(max(t0, 0) // fr, bool), op])
    look = lag_frames * fr // 2 + look_extra   # open a little ahead of the decision's frame (attack ends at the onset)
    g = G.fast_ramp(opn, fr, 0, 96, int(release_ms * 48), rng_db)
    g = np.concatenate([g[look:], np.ones(look) * g[-1]]) if look > 0 else g
    n = len(mic)
    g = np.concatenate([g, np.full(max(0, n - len(g)), g[-1])])[:n]
    # decay-following release: for 400 ms after each closing, the gain may not fall below the Wiener gain of the
    # signal against the background floor - a word's tail fades as it meets the floor instead of a fixed ramp
    pw = sg.lfilter([1 - 0.98], [1, -0.98], mic ** 2)                    # ~1 ms power envelope
    # background floor: minimum statistics of the frame level (the same tracker the detector uses), per 2 ms frame
    fl = 10 ** (V.floor_track(Fk[:, 51]) / 10)
    fl = np.concatenate([np.full(max(t0, 0) // fr, fl[0]), fl])
    floor = np.repeat(fl, fr); floor = np.concatenate([floor, np.full(max(0, n - len(floor)), floor[-1])])[:n]
    # the frame level is a 12 ms window sum with Hann weighting; scale to the 1 ms power envelope's units
    floor = floor * np.mean(pw) / np.mean(np.repeat(10 ** (Fk[:, 51] / 10), fr)[:n] + 1e-30)
    wien = np.sqrt(np.clip(1 - wiener_k * floor / (pw + 1e-20), 0, 1))
    closing = np.zeros(n, bool)
    dg = np.diff(np.concatenate([[1.0], (g > 0.5).astype(float)]))
    for k in np.where(dg < 0)[0]:
        closing[k:k + int(follow_ms / 1000 * C.FS)] = True
    return np.where(closing, np.maximum(g, wien), g)

if __name__ == "__main__":
    takes = []
    for spk in V.SPK:
        for k in range(3):
            rng = np.random.default_rng(zlib.crc32(f"bench/{spk}/{k}".encode()))
            d = C.take(spk, 10, seed=5000 + k, bleed_db=rng.uniform(-36, -20), noise_db=rng.uniform(-68, -54), breaths=True)
            takes.append((spk, k, d))
    models = {}
    for te in V.SPK:
        tr = np.where(V.SS != te)[0][::2]
        models[te] = V.fit(V.X[tr], V.Y[tr])
    rows = []
    for spk, k, d in takes:
        Fk = features(d["mic"], f"{spk}{k}")
        res = {}
        # classic gate: exhaustive search; the best by a combined score (voice loss + onset loss weighted, background)
        flo = 10 * np.log10(np.mean((d["bleed"] + d["noise"]) ** 2))
        cc = f"{TMP}/{spk}{k}_classic.npy"
        if os.path.exists(cc):
            classic = list(np.load(cc, allow_pickle=True))
        else:
            classic = []
            for thr, look, hold, rel in itertools.product(np.arange(flo + 3, flo + 40, 3), (0, 2, 5, 10), (20, 80, 200), (40, 120, 300)):
                g = G.classic_gain(d["mic"], thr, look_ms=look, hold_ms=hold, release_ms=rel)
                classic.append((G.score(g, d), (thr - flo, look, hold, rel)))
            np.save(cc, np.array(classic, dtype=object), allow_pickle=True)
        for kb in (False, True):
            g = smart_gain(d["mic"], Fk, models[spk], keep_breath=kb)
            res[f"Axell smart ({'keep' if kb else 'remove'} breaths)"] = G.score(g, d)
        rows.append((spk, k, d["bleed_db"], classic, res))
        print(f"\n{spk} take {k} (bleed {d['bleed_db']:.0f} dB re beat)")
        for name, s in res.items():
            # the classic gate at the same (or more) background removal: its least voice damage, and its setting
            ok = [c for c in classic if c[0]["bg"] <= s["bg"] + 0.5]
            if ok:
                bv = min(ok, key=lambda c: c[0]["voice"] + 0.5 * c[0]["onset"])
                cl = f"classic at >= this removal: voice {bv[0]['voice']:6.1f}, starts {bv[0]['onset']:6.1f} (thr floor+{bv[1][0]:.0f}, look {bv[1][1]}, hold {bv[1][2]}, rel {bv[1][3]})"
            else:
                cl = f"classic: NO setting removes this much (best {min(c[0]['bg'] for c in classic):.1f} dB)"
            print(f"  {name:32s} voice {s['voice']:6.1f} | starts {s['onset']:6.1f} | bg left {s['bg']:6.1f} | breaths {s['breath']:6.1f}  ||  {cl}")
    np.save(f"{TMP}/vocal_rows.npy", np.array(rows, dtype=object), allow_pickle=True)
