"""Vocal gate simulation and scoring on takes with ground truth.

Output of every gate = g(n) * mic(n); with the components known, the damage is exact:
  voice loss      10 log10( sum((1-g) v)^2 / sum v^2 )                (lower = better; whole take)
  onset loss      the same over the first 40 ms of every voice segment (word starts: what a gate chops first)
  background      10 log10( sum (g i)^2 / sum i^2 ) over pauses, i = bleed + noise (lower = more removed)
  breaths         10 log10( sum (g b)^2 / sum b^2 )  (reported; the target depends on the BREATH setting)

CLASSIC: the level gate every DAW and plugin has: peak level of the (100 Hz high-passed) side chain in 1 ms frames,
threshold + hysteresis, look-ahead, attack / hold / release, range; searched over its whole parameter grid per take.

SMART (Axell): the voice-activity network (vad_train.py) -> a two-state hidden Markov model smoothed with fixed lag
(the look-ahead): P(voice at t | features up to t + lag) -> open when > 0.5 -> gain ramps aligned with the look-ahead;
closing follows the signal's own decay into the background floor (decay-following release)."""
import os, sys, numpy as np
from scipy import signal as sg
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus as C

FS = C.FS
HOP = 96   # 2 ms (VelvetFeatures at 48 kHz)

def segments(mask):
    d = np.diff(np.concatenate([[0], mask.astype(int), [0]]))
    return list(zip(np.where(d == 1)[0], np.where(d == -1)[0]))

def score(g, d):
    v, i, b, vl = d["voice"], d["bleed"] + d["noise"], d["breath"], d["vlab"]
    n = min(len(g), len(v)); g = g[:n]; v, i, b, vl = v[:n], i[:n], b[:n], vl[:n]
    db = lambda a, c: 10 * np.log10((a + 1e-30) / (c + 1e-30))
    on = np.zeros(n, bool)
    for a, e in segments(vl):
        on[a:min(e, a + int(0.04 * FS))] = True
    pause = ~vl & ~d["blab"][:n]
    # pauses: only well away from the voice (a gate is allowed its release time after a word)
    far = sg.convolve(vl.astype(float), np.ones(int(0.25 * FS)), mode="same") == 0
    return dict(voice=db(np.sum(((1 - g) * v) ** 2), np.sum(v ** 2)),
                onset=db(np.sum(((1 - g) * v)[on] ** 2), np.sum(v[on] ** 2)),
                bg=db(np.sum((g * i)[pause & far] ** 2), np.sum(i[pause & far] ** 2)),
                breath=db(np.sum((g * b) ** 2), np.sum(b ** 2)) if np.any(b) else np.nan)

def ramp_gain(open_frames, fr, look, attack, release, rng_db, decay_follow=None):
    """per-sample gain from per-frame open decisions (fr samples per frame): open ramps (raised cosine, `attack`
    samples) placed to END at the frame's start shifted by `look` samples earlier; release raised cosine of `release`
    samples, or (decay_follow) a gain that follows the signal's own decay into the floor"""
    n = len(open_frames) * fr
    floor = 10 ** (rng_db / 20)
    tgt = np.repeat(open_frames.astype(float), fr)
    # look-ahead: decisions apply `look` samples earlier
    tgt = np.concatenate([tgt[look:], np.zeros(look)])
    g = np.empty(n); cur = floor
    up = 1.0 / max(attack, 1); down = 1.0 / max(release, 1)
    ph = 1.0  # 0..1 position of the cosine ramp
    state = 0.0
    for k in range(n):
        if tgt[k] > 0.5:
            state = min(1.0, state + up)
        else:
            state = max(0.0, state - down)
        s = 0.5 - 0.5 * np.cos(np.pi * state)
        g[k] = floor + (1 - floor) * s
    if decay_follow is not None:
        # while closing, never below the signal-to-floor Wiener gain: the tail fades as it meets the background
        g = np.maximum(g, np.minimum(1.0, decay_follow[:n]) * (1 - floor) + floor) if False else g
    return g

def classic_gain(mic, thr_db, hyst_db=4.0, look_ms=2.0, attack_ms=0.5, hold_ms=50.0, release_ms=120.0, rng_db=-60.0):
    fr = 48
    hp = sg.lfilter(*sg.butter(2, 100, "high", fs=FS), mic)
    n = len(hp) // fr * fr
    pk = 20 * np.log10(np.abs(hp[:n]).reshape(-1, fr).max(1) + 1e-12)
    op = np.zeros(len(pk), bool); st = False; hold = 0
    hfr = int(hold_ms / 1000 * FS / fr)
    for k, v in enumerate(pk):
        if v > thr_db:
            st = True; hold = hfr
        elif v < thr_db - hyst_db:
            if hold > 0:
                hold -= 1
            else:
                st = False
        op[k] = st
    return fast_ramp(op, fr, int(look_ms * 48), int(attack_ms * 48), int(release_ms * 48), rng_db)

def fast_ramp(op, fr, look, attack, release, rng_db):
    """vectorised ramp: linear state integrator with per-sample slopes via cumulative trick on runs"""
    tgt = np.repeat(op.astype(float), fr)
    tgt = np.concatenate([tgt[look:], np.zeros(look)])
    n = len(tgt); state = np.empty(n)
    up = 1.0 / max(attack, 1); down = 1.0 / max(release, 1)
    s = 0.0
    # run-wise: within a run of constant target the state moves linearly until it saturates
    edges = np.concatenate([[0], np.where(np.diff(tgt) != 0)[0] + 1, [n]])
    for a, b in zip(edges[:-1], edges[1:]):
        L = b - a
        if tgt[a] > 0.5:
            seq = np.minimum(1.0, s + up * np.arange(1, L + 1))
        else:
            seq = np.maximum(0.0, s - down * np.arange(1, L + 1))
        state[a:b] = seq; s = seq[-1]
    floor = 10 ** (rng_db / 20)
    return floor + (1 - floor) * (0.5 - 0.5 * np.cos(np.pi * state))
