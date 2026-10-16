"""Voice-activity network: Velvet's frame features + level above a tracked noise floor + 48 ms of past context ->
3 classes (background, voice, breath). Leave-one-speaker-out evaluation; the final model is written to
Source/VoiceModel.h by write_header()."""
import os, sys, numpy as np
from collections import deque
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

D = np.load(os.path.expanduser("~/axell-gate-data/vad/vad.npz"), allow_pickle=True)
F, L, S, K = D["F"].astype(np.float64), D["L"], D["S"], D["K"]
SPK = ("slt", "bdl", "jmk")

def floor_track(level_db, win=750, smooth=10):
    """noise floor: the minimum of the 20 ms-smoothed frame level over the past 1.5 s (minimum statistics)"""
    out = np.empty_like(level_db); q = deque(); s = level_db[0]
    for i, v in enumerate(level_db):
        s += (v - s) / smooth
        while q and q[-1][1] >= s:
            q.pop()
        q.append((i, s))
        while q[0][0] <= i - win:
            q.popleft()
        out[i] = q[0][1]
    return out

def past_mean(x, k=24):
    c = np.cumsum(np.vstack([np.zeros((1, x.shape[1])), x]), 0)
    i = np.arange(len(x)); lo = np.maximum(0, i - k + 1)
    return (c[i + 1] - c[lo]) / (i + 1 - lo)[:, None]

def inputs(Fk):
    """Fk: the VelvetFeatures rows of one take -> model inputs"""
    feat, adapted = Fk[:, 1:11], Fk[:, 11:21]
    level, ap, psib = Fk[:, 51], Fk[:, 53], Fk[:, 54]
    reFloor = level - floor_track(level)
    base = np.column_stack([feat, adapted[:, 3:10], reFloor, psib, (ap < 0.3).astype(float)])
    ctx = past_mean(np.column_stack([ap, reFloor, feat[:, 9]]))
    return np.column_stack([base, ctx])

X = np.concatenate([inputs(F[(S == s) & (K == k)]) for s in SPK for k in np.unique(K[S == s])])
Y = np.concatenate([L[(S == s) & (K == k)] for s in SPK for k in np.unique(K[S == s])])
SS = np.concatenate([S[(S == s) & (K == k)] for s in SPK for k in np.unique(K[S == s])])

def fit(X, y, H=32, epochs=10, seed=0, l2=1e-4, C3=3):
    rng = np.random.default_rng(seed); mu, sd = X.mean(0), X.std(0) + 1e-9; Z = (X - mu) / sd
    P = [rng.standard_normal((Z.shape[1], H)) / np.sqrt(Z.shape[1]), np.zeros(H), rng.standard_normal((H, C3)) / np.sqrt(H), np.zeros(C3)]
    M = [np.zeros_like(q) for q in P]; V = [np.zeros_like(q) for q in P]; t = 0; lr = 3e-3
    Yo = np.eye(C3)[y]
    for ep in range(epochs):
        idx = rng.permutation(len(Z))
        for s0 in range(0, len(Z), 4096):
            b = idx[s0:s0 + 4096]; z = Z[b]; h = np.tanh(z @ P[0] + P[1]); o = h @ P[2] + P[3]
            o -= o.max(1, keepdims=True); p = np.exp(o); p /= p.sum(1, keepdims=True)
            g = (p - Yo[b]) / len(b); gh = (g @ P[2].T) * (1 - h * h)
            G = [z.T @ gh + l2 * P[0], gh.sum(0), h.T @ g + l2 * P[2], g.sum(0)]; t += 1
            for i in range(4):
                M[i] = 0.9 * M[i] + 0.1 * G[i]; V[i] = 0.999 * V[i] + 0.001 * G[i] ** 2
                P[i] -= lr * (M[i] / (1 - 0.9 ** t)) / (np.sqrt(V[i] / (1 - 0.999 ** t)) + 1e-8)
        lr *= 0.75
    return P, mu, sd

def prob(X, m):
    P, mu, sd = m; h = np.tanh(((X - mu) / sd) @ P[0] + P[1]); o = h @ P[2] + P[3]
    o -= o.max(1, keepdims=True); p = np.exp(o); return p / p.sum(1, keepdims=True)

if __name__ == "__main__":
    print(f"{len(X)} frames, {X.shape[1]} inputs")
    for te in SPK:
        tr = np.where(SS != te)[0][::2]
        m = fit(X[tr], Y[tr])
        sel = SS == te; p = prob(X[sel], m); y = Y[sel]
        pv = p[:, 1]
        acc = ((pv > 0.5) == (y == 1)).mean()
        miss = (pv[y == 1] < 0.5).mean(); fa_bg = (pv[y == 0] > 0.5).mean(); fa_br = (pv[y == 2] > 0.5).mean()
        br_rec = (p[y == 2, 2] > 0.5).mean()
        print(f"held-out {te}: frame accuracy {100 * acc:.1f} % | voice missed {100 * miss:.1f} % | background taken as voice {100 * fa_bg:.1f} % | "
              f"breath taken as voice {100 * fa_br:.1f} % | breath recognised {100 * br_rec:.1f} %")
