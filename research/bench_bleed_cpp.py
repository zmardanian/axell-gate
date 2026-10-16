"""End-to-end vocal gate with and without BLEED (the beat on the side-chain), through the C++ engine.
Measures on the output, aligned with the clean components:
  pauses   what is left of bleed + noise in the pauses, re what came in (dB)
  speech   the total error during speech (residual bleed + noise + any damage to the voice), re the voice (dB)
usage: python3 bench_bleed_cpp.py <GateRender>"""
import os, sys, zlib, subprocess, struct, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus as C
from bench_vocal_cpp import wavf
TOOL = sys.argv[1] if len(sys.argv) > 1 else ""
TMP = os.path.expanduser("~/axell-gate-data/bench")
def wav2(p, x):
    d = np.asarray(x, "<f4").tobytes()
    with open(p, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(d)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 3, 2, C.FS, C.FS * 8, 8, 32) + b"data" + struct.pack("<I", len(d)) + d)
db = lambda x: 10 * np.log10(np.sum(x ** 2) + 1e-30)
def main():
    for spk in ("slt", "bdl", "jmk"):
      rng = np.random.default_rng(zlib.crc32(f"bleed/{spk}".encode()))
      d = C.take(spk, 24, seed=7000, bleed_db=-24.0, noise_db=-70.0, breaths=False)
      wavf(f"{TMP}/bm.wav", d["mic"]); wav2(f"{TMP}/bs.wav", d["beat"])
      res = {}
      for name, extra in (("gate", []), ("gate + BLEED", ["bleed=1", f"sc={TMP}/bs.wav"])):
          r = subprocess.run([TOOL, f"{TMP}/bm.wav", f"{TMP}/bo.wav"] + extra, capture_output=True, text=True, check=True)
          y = np.fromfile(f"{TMP}/bo.wav", np.float32)[11:][:len(d["mic"])].astype(float)
          n = len(y); v = d["vlab"][:n]; half = np.arange(n) > n // 2
          far = np.convolve(v.astype(float), np.ones(int(0.25 * C.FS)), mode="same") == 0
          interf = (d["bleed"] + d["noise"])[:n]
          res[name] = (db(y[half & far]) - db(interf[half & far]), db((y - d["voice"][:n])[half & v]) - db(d["voice"][:n][half & v]), r.stdout.split(";")[0])
      print(f"{spk} (bleed {db(d['bleed'][d['vlab']]) - db(d['voice'][d['vlab']]):.0f} dB re voice): " + " | ".join(
          f"{k}: pauses {a:6.1f} dB, speech error {b:6.1f} dB [{c}]" for k, (a, b, c) in res.items()))


if __name__ == "__main__":
    main()
