"""How much beat bleed the canceller removes (ERLE) in the pauses and under the voice, and what it does to the voice."""
import sys, numpy as np
sys.path.insert(0, ".")
import corpus as C
from canceller import Canceller

def db(x):
    return 10 * np.log10(np.mean(x ** 2) + 1e-30)

for spk, seed in (("slt", 1), ("bdl", 2), ("jmk", 3)):
    d = C.take(spk, 8, seed=seed)
    can = Canceller()
    mode = sys.argv[1] if len(sys.argv) > 1 else "auto"
    adapt = None
    if mode == "oracle":   # learn only where the voice is absent (the delayed mic's labels)
        vl = np.concatenate([np.zeros(can.lead, bool), d["vlab"]])[:len(d["vlab"])]
        adapt = (~vl).astype(float)
    out, est = can.process(d["mic"], d["beat"], adapt)
    L = can.lead; n = len(out)
    voice = np.concatenate([np.zeros(L), d["voice"]])[:n]; bleed = np.concatenate([np.zeros(L), d["bleed"]])[:n]
    other = np.concatenate([np.zeros(L), d["noise"] + d["breath"]])[:n]
    v = np.concatenate([np.zeros(L, bool), d["vlab"]])[:n]
    resid = out - voice - other                        # what is left of the bleed (plus any damage to the voice)
    half = n // 2                                      # second half: converged
    t = np.arange(n) > half
    print(f"{spk}: bleed {d['bleed_db']:.1f} dB re beat | ERLE pauses {db(bleed[t & ~v]) - db(resid[t & ~v]):5.1f} dB, under the voice "
          f"{db(bleed[t & v]) - db(resid[t & v]):5.1f} dB | first 2 s {db(bleed[:96000]) - db(resid[:96000]):5.1f} dB | "
          f"voice: residual re voice {db(resid[t & v]) - db(voice[t & v]):6.1f} dB | leak est {can.leak:.3f}")
