"""Test material with known ground truth for the gate research.

VOCAL TAKES: CMU ARCTIC utterances (clean, phone-labelled) strung into rap-like takes at 48 kHz with natural pauses,
plus, recorded "in the room":
  - BLEED: a synthetic beat played on closed headphones and leaking into the vocal mic. Leak model: each ear cup a direct
    path (0.2-0.8 ms) with the closed-back leak response (bass leaks little: 2nd-order high-pass ~250 Hz, a broad
    presence bump, a 10-12 kHz roll-off), plus a few room reflections (1.5-18 ms, -8 to -20 dB); overall -22 to -36 dB
    re the beat's level, with a slow +-2 dB drift (head movement). Different for left and right cup.
  - NOISE: pink room noise and mains hum at a fixed floor.
  - ROOM (optional, `room=` dict; 2.0): the rooms real takes come from - no bleed, room tone of any colour (white to
    brown), no hum, sub-60 Hz rumble that can be louder than the room tone, slowly breathing air conditioning, dense
    rap pauses, and a fast compressor on the whole chain (it lifts the room between words).
  - REVERB (optional, room `rt60`; 2.0): an untreated room - direct sound, strong early reflections (floor, desk, bare
    walls) and a late tail that rings longer in the bass. Scoring: direct sound + the first 20 ms are the voice; the
    late tail is the room (background): a gate that cuts it between words removes room, not voice.
  - BREATHS: inhale-like noise bursts (0.25-0.55 s, band 300 Hz - 6 kHz) in some pauses.
Every component is returned separately: voice, bleed, noise, breath, the beat (the reference the plugin receives) and
per-sample labels (voice activity from the phone labels, breath activity).

BEATS: a synthetic 4-bar beat (kick, snare, hats, 808, chords) for the bleed reference and the drum tests."""
import os, sys, zlib, numpy as np
from scipy import signal as sg
sys.path.insert(0, os.path.expanduser("~/axell-velvet/research"))
from data import utterances as arctic_utterances, load as arctic_load  # noqa: E402

# 2.0: VCTK voices (research/vctk_fetch.py; CC BY 4.0) next to CMU ARCTIC's. Speaker ids "p..." are VCTK.
VCTK = os.path.expanduser("~/axell-gate-data/vctk")
# 3.0 (AXG_VCTK3=1): Axell Velvet 3.0's VCTK set - 108 voices, 60 utterances each, phone labels by the Montreal Forced
# Aligner (axell-velvet research/vctk_fetch.py, corpus3.py; MFA agrees with CMU ARCTIC's labels on voice activity at
# 96.3 %, as 2.0's level labeller did, with no "don't care" frames). The 12 voices of corpus3.TEST_VCTK are never
# trained on (AXG_VCTK3_TEST=1 lists only them, for the held-out benchmark).
V3 = os.environ.get("AXG_VCTK3") == "1"
VCTK3 = os.path.expanduser("~/axell-velvet-data/vctk")
ALIGNED3 = os.path.expanduser("~/axell-velvet-data/vctk_aligned")
TEST_VCTK3 = ['p335', 'p280', 'p253', 'p248', 'p323', 'p351', 'p326', 'p376', 'p347', 'p304', 'p363', 'p298']   # corpus3.TEST_VCTK

def vctk_speakers(gender=None):
    if V3:
        rows = [l.split() for l in open(f"{VCTK3}/speakers.txt") if l.strip()]
        test = os.environ.get("AXG_VCTK3_TEST") == "1"
        return [r[0] for r in rows if (gender is None or r[1] == gender) and os.path.isdir(f"{ALIGNED3}/{r[0]}")
                and ((r[0] in TEST_VCTK3) == test)]
    if not os.path.exists(f"{VCTK}/speakers.txt"):
        return []
    rows = [l.split() for l in open(f"{VCTK}/speakers.txt") if l.strip()]
    return [r[0] for r in rows if gender is None or r[1] == gender]

def _textgrid_phones(p):
    """MFA phones: silence ("", sil, sp) -> "pau"; every phone and spoken noise (spn) is voice"""
    L = open(p).read().splitlines(); out = []; tier = None; i = 0
    while i < len(L):
        t = L[i].strip()
        if t.startswith("name ="):
            tier = t.split("=")[1].strip().strip('"')
        if tier == "phones" and t.startswith("xmin =") and i + 2 < len(L) and L[i + 2].strip().startswith("text ="):
            a = float(t.split("=")[1]); b = float(L[i + 1].split("=")[1]); q = L[i + 2].split("=", 1)[1].strip().strip('"')
            out.append((a, b, "pau" if q in ("", "sil", "sp") else q)); i += 3; continue
        i += 1
    return out

def _float_wav(p):
    import struct
    d = open(p, "rb").read(); i = 12; fs = 48000; ch = 1
    while i < len(d) - 8:
        cid = d[i:i + 4]; n = struct.unpack("<I", d[i + 4:i + 8])[0]
        if cid == b"fmt ":
            ch = struct.unpack("<H", d[i + 10:i + 12])[0]; fs = struct.unpack("<I", d[i + 12:i + 16])[0]
        elif cid == b"data":
            return fs, np.frombuffer(d[i + 8:i + 8 + n], "<f4").reshape(-1, ch)[:, 0].astype(np.float64)
        i += 8 + n + (n & 1)
    raise ValueError(p)

def vctk_labels(x, fs):
    """voice activity of a clean close-mic studio recording from its level. VCTK's floor is not silence (often
    ~30 dB under the voice, wobbling), so: 5 ms frames above max(the typical floor (20th percentile) + 9 dB,
    peak - 45 dB); gaps under 150 ms filled (phone labels mark only real pauses); islands under 50 ms, or under
    120 ms and 20 dB below the peak (clicks, noise), dropped; each segment starts 20 ms earlier (soft onsets).
    Calibrated against CMU ARCTIC's phone labels with a VCTK-like floor added (180 utterances): 96.0 % of the time
    agrees, 3.9 % of the voice missed, 4.9 % of the pauses marked voice (the first version missed 21 %).
    Returned as phone-like segments ("v" / "pau"), plus the filled gaps as "gap" segments: short pauses between words
    hold only bleed and room there; calling them voice would teach "bleed = voice", calling them pause would fight
    the phone-label convention - training leaves them out ("don't care")"""
    fr = int(0.005 * fs); n = len(x) // fr
    lev = 10 * np.log10(np.mean(x[:n * fr].reshape(n, fr) ** 2, 1) + 1e-14)
    pk = np.percentile(lev, 99)
    thr = max(np.percentile(lev, 20) + 6.0, pk - 45.0)
    on = lev > thr
    def runs(m):
        d = np.diff(np.concatenate([[0], m.astype(int), [0]]))
        return list(zip(np.where(d == 1)[0], np.where(d == -1)[0]))
    gap = np.zeros(n, bool)
    for a, b in runs(~on):
        if a > 0 and b < n and b - a < 30:
            on[a:b] = True; gap[a:b] = True
    for a, b in runs(on):
        if b - a < 10 or (b - a < 24 and lev[a:b].max() < pk - 20.0):
            on[a:b] = False
    for a, b in runs(on):
        on[max(0, a - 4):a] = True
    gap &= on
    segs, t = [], 0.0
    for a, b in runs(on):
        if a * fr / fs > t:
            segs.append((t, a * fr / fs, "pau"))
        segs.append((a * fr / fs, b * fr / fs, "v")); t = b * fr / fs
    segs.append((t, len(x) / fs, "pau"))
    # the filled gaps once more, as "gap": voice for the take's labels, but left out of training (see take)
    segs += [(a * fr / fs, b * fr / fs, "gap") for a, b in runs(gap)]
    return segs

def utterances(spk):
    if spk.startswith("p") and V3:
        return [(f[:-9], f"{VCTK3}/{spk}/{f[:-9]}.wav", f"{ALIGNED3}/{spk}/{f}") for f in sorted(os.listdir(f"{ALIGNED3}/{spk}"))
                if f.endswith(".TextGrid") and os.path.exists(f"{VCTK3}/{spk}/{f[:-9]}.wav")]
    if spk.startswith("p"):
        d = f"{VCTK}/{spk}"
        return [(f[:-4], f"{d}/{f}", None) for f in sorted(os.listdir(d)) if f.endswith(".wav")]
    return list(arctic_utterances(spk))

def load(spk, w, l):
    if spk.startswith("p") and V3:
        # 3.0: MFA's phones and the 2.0 level labeller must agree; where they differ, "don't care" ("gap"). MFA alone
        # stretches words into the pauses around them (7.2 % of ARCTIC's pause frames marked voice): trained on it,
        # the gate opened in 15 % of the pauses of the artist's setup instead of 4.6 % (DECISIONS G23)
        fs, x = _float_wav(w)
        H = 0.005; n = int(len(x) / fs / H) + 1; t = (np.arange(n) + 0.5) * H
        mfa = np.zeros(n, bool); lev = np.zeros(n, bool)
        for a, b, q in _textgrid_phones(l):
            if q != "pau":
                mfa |= (t >= a) & (t < b)
        gapm = np.zeros(n, bool)
        for a, b, q in vctk_labels(x, fs):
            if q == "v":
                lev |= (t >= a) & (t < b)
            elif q == "gap":
                gapm |= (t >= a) & (t < b)
        # the short gaps between words (< 150 ms; the level labeller fills them) stay "don't care" whatever MFA says:
        # in a take they hold only bleed and room, and labelled voice they teach "bleed between words = voice" (G19)
        lab = np.where(gapm, 2, np.where(mfa & lev, 1, np.where(~mfa & ~lev, 0, 2)))
        segs, a = [], 0
        for i in range(1, n + 1):
            if i == n or lab[i] != lab[a]:
                segs.append((a * H, i * H, ("pau", "v", "gap")[lab[a]])); a = i
        return fs, x, None, segs
    if spk.startswith("p"):
        import wave
        f = wave.open(w); fs = f.getframerate()
        x = np.frombuffer(f.readframes(f.getnframes()), np.int16).astype(np.float64) / 32768.0
        return fs, x, None, vctk_labels(x, fs)
    return arctic_load(spk, w, l)

FS = 48000

def pink(n, rng):
    """pink noise by the Voss-McCartney-free method: white through a -3 dB/oct filter (Kellet's coefficients)"""
    w = rng.standard_normal(n)
    b = [0.049922035, -0.095993537, 0.050612699, -0.004408786]
    a = [1, -2.494956002, 2.017265875, -0.522189400]
    return sg.lfilter(b, a, w)

def beat(secs, bpm=90.0, seed=1):
    """a stereo hip-hop beat: kick + 808 on 1 and the "and" of 2, snare on 2 and 4, 16th hats with velocity, minor-7 chords.
    Returns (stereo array, dict of the stems)."""
    rng = np.random.default_rng(seed)
    n = int(secs * FS); t = np.arange(n) / FS
    beat_s = 60.0 / bpm; sixteenth = beat_s / 4
    stems = {k: np.zeros(n) for k in ("kick", "snare", "hat", "808", "keys")}
    def put(name, x, at, pan=0.0):
        i = int(at * FS)
        if i >= n:
            return
        m = min(len(x), n - i); stems[name][i:i + m] += x[:m]
    bar = 4 * beat_s
    for b0 in np.arange(0, secs, bar):
        for k in (0.0, 1.5 * beat_s, 2.75 * beat_s):                         # kick
            tt = np.arange(int(0.35 * FS)) / FS
            f = 45 + 110 * np.exp(-tt * 35)
            put("kick", np.sin(2 * np.pi * np.cumsum(f) / FS) * np.exp(-tt * 9) + 0.3 * np.exp(-tt * 400) * rng.standard_normal(len(tt)), b0 + k)
            tt = np.arange(int(1.2 * FS)) / FS                              # 808 under the kick
            f = 49 * (1 + 0.15 * np.exp(-tt * 20))
            put("808", np.tanh(1.5 * np.sin(2 * np.pi * np.cumsum(f) / FS)) * np.exp(-tt * 2.2), b0 + k)
        for k in (beat_s, 3 * beat_s):                                       # snare
            tt = np.arange(int(0.3 * FS)) / FS
            nz = sg.lfilter(*sg.butter(2, [1500, 9000], "bandpass", fs=FS), rng.standard_normal(len(tt)))
            put("snare", 0.8 * nz * np.exp(-tt * 18) + 0.5 * np.sin(2 * np.pi * 185 * tt) * np.exp(-tt * 30), b0 + k)
        for s in range(16):                                                  # hats
            tt = np.arange(int(0.08 * FS)) / FS
            nz = sg.lfilter(*sg.butter(2, 7000, "high", fs=FS), rng.standard_normal(len(tt)))
            put("hat", (0.35 if s % 2 else 0.6) * nz * np.exp(-tt * 70), b0 + s * sixteenth)
        tt = np.arange(int(bar * FS)) / FS                                   # chords (Am7 / Fmaj7)
        chord = [220.0, 261.6, 329.6, 392.0] if int(b0 / bar) % 2 == 0 else [174.6, 220.0, 261.6, 329.6]
        k = sum(np.sin(2 * np.pi * f * tt + rng.uniform(0, 6)) + 0.3 * np.sin(4 * np.pi * f * tt) for f in chord)
        put("keys", 0.08 * k * (0.6 + 0.4 * np.exp(-np.mod(tt, beat_s) * 3)), b0)
    mix = stems["kick"] * 0.9 + stems["808"] * 0.7 + stems["snare"] * 0.7 + stems["hat"] * 0.35 + stems["keys"]
    L = mix + 0.1 * stems["hat"] - 0.05 * stems["keys"]; R = mix - 0.1 * stems["hat"] + 0.05 * stems["keys"]
    st = np.stack([L, R], 1)
    st *= 10 ** (-8 / 20) / np.max(np.abs(st))
    return st, stems

def headphone_leak(rng):
    """impulse responses (left cup, right cup) from the headphone drivers to the vocal mic"""
    irs = []
    for cup in range(2):
        L = int(0.025 * FS); h = np.zeros(L)
        h[int(rng.uniform(0.2, 0.8) * 1e-3 * FS)] = 1.0
        for _ in range(rng.integers(3, 7)):
            h[int(rng.uniform(1.5, 18) * 1e-3 * FS)] += rng.choice([-1, 1]) * 10 ** (rng.uniform(-20, -8) / 20)
        h = sg.lfilter(*sg.butter(2, rng.uniform(180, 350), "high", fs=FS), h)              # closed back: little bass
        w0 = 2 * np.pi * rng.uniform(1800, 3500) / FS; r = 0.93                           # broad presence bump
        h = h + 0.6 * sg.lfilter([1 - r], [1, -2 * r * np.cos(w0), r * r], h)
        h = sg.lfilter(*sg.butter(2, rng.uniform(9000, 12500), "low", fs=FS), h)
        irs.append(h / np.sqrt(np.sum(h ** 2)))
    return irs

def coloured(n, beta, rng, f_lo=20.0):
    """noise with a power spectrum ~ 1/f^beta (0 white, 1 pink, 2 brown), flat below f_lo"""
    W = np.fft.rfft(rng.standard_normal(n)); f = np.fft.rfftfreq(n, 1 / FS)
    W *= np.maximum(f, f_lo) ** (-beta / 2); W[0] = 0
    return np.fft.irfft(W, n)

def compress(x, thr_db, ratio, att_ms=1.0, rel_ms=60.0):
    """the gain of a fast feed-forward peak compressor on x (1176-like attack and release)"""
    a = np.exp(-1 / (att_ms * 1e-3 * FS)); r = np.exp(-1 / (rel_ms * 1e-3 * FS))
    lev = 20 * np.log10(np.abs(x) + 1e-12); over = np.maximum(lev - thr_db, 0)
    env = []; e = 0.0
    for v in over.tolist():
        e = a * e + (1 - a) * v if v > e else r * e + (1 - r) * v
        env.append(e)
    return 10 ** (-np.array(env) * (1 - 1 / ratio) / 20)

def room_ir(rng, rt60, lf_factor=1.4, drr_db=10.0):
    """an untreated room's response at a vocal mic: (early part: direct + reflections up to 20 ms, late part).
    Late tail: noise decaying 60 dB in rt60 above 300 Hz and in rt60 * lf_factor below; direct-to-late ratio drr_db"""
    n = int(1.3 * rt60 * lf_factor * FS) + 1
    h = np.zeros(n); h[0] = 1.0
    for _ in range(rng.integers(4, 9)):
        h[int(rng.uniform(1.5, 15) * 1e-3 * FS)] += rng.choice([-1, 1]) * 10 ** (rng.uniform(-14, -5) / 20)
    t = np.arange(n) / FS
    lf = sg.sosfilt(sg.butter(2, 300, "low", fs=FS, output="sos"), rng.standard_normal(n)) * np.exp(-6.91 * t / (rt60 * lf_factor))
    hf = sg.sosfilt(sg.butter(2, 300, "high", fs=FS, output="sos"), rng.standard_normal(n)) * np.exp(-6.91 * t / rt60)
    late = (lf + hf) * np.clip((t - 0.004) / 0.01, 0, 1)                   # builds up after the first reflections
    late *= np.sqrt(np.sum(h ** 2) / np.sum(late ** 2)) * 10 ** (-drr_db / 20)
    full = h + late
    cut = int(0.02 * FS)
    early = full.copy(); early[cut:] = 0.0
    tail = full.copy(); tail[:cut] = 0.0
    return early, tail

def take(spk="slt", n_utt=10, seed=0, bleed_db=None, noise_db=-62.0, breaths=True, room=None):
    """one vocal take; levels re full scale: the voice is normalised to -18 dBFS RMS on its active parts.
    room (optional): bleed (bool), gap (lo, hi seconds), tilt (noise 1/f^tilt), hum (bool), rumble_db (absolute RMS
    of the 4-60 Hz rumble, or None), hvac_db (slow modulation depth of the room tone), comp (ratio or None, with
    comp_thr_db re the voice's RMS), rt60 (seconds, untreated-room reverb on the voice and breaths; with
    rt60_lf (the bass's factor) and drr_db)"""
    rng = np.random.default_rng(zlib.crc32(f"take/{spk}/{seed}".encode()))
    rr = np.random.default_rng(zlib.crc32(f"room/{spk}/{seed}".encode()))
    room = room or {}
    glo, ghi = room.get("gap", (0.35, 1.4))
    utts = list(utterances(spk)); idx = rng.choice(len(utts), n_utt, replace=False)
    parts, labels, pauses, ignores = [], [], [], []
    for k, i in enumerate(idx):
        name, w, l = utts[i]
        fs0, x, egg, ph = load(spk, w, l)
        if fs0 != FS:
            from math import gcd
            gg = gcd(FS, fs0); x = sg.resample_poly(x, FS // gg, fs0 // gg)
        lab = np.zeros(len(x), bool); ign = np.zeros(len(x), bool)
        for a, b, p in ph:
            if p == "gap":
                ign[int(a * FS):int(b * FS)] = True
            elif p != "pau":
                lab[int(a * FS):int(b * FS)] = True
        # trim the utterance's own leading/trailing silence to 50 ms around speech
        on = np.where(lab)[0]
        a = max(0, on[0] - int(0.05 * FS)); b = min(len(x), on[-1] + int(0.05 * FS))
        g = 10 ** (rng.uniform(-4, 4) / 20)
        parts.append(x[a:b] * g); labels.append(lab[a:b]); ignores.append(ign[a:b])
        gap = int(rng.uniform(glo, ghi) * FS)
        parts.append(np.zeros(gap)); labels.append(np.zeros(gap, bool)); ignores.append(np.zeros(gap, bool)); pauses.append((sum(len(p) for p in parts) - gap, gap))
    voice = np.concatenate(parts); vlab = np.concatenate(labels); xlab = np.concatenate(ignores)
    voice *= 10 ** (-18 / 20) / np.sqrt(np.mean(voice[vlab] ** 2))
    n = len(voice)
    # the beat on the headphones, leaking into the mic
    bt, _ = beat(n / FS + 0.1, seed=seed + 7)
    bt = bt[:n]
    irs = headphone_leak(rng)
    leak = sg.fftconvolve(bt[:, 0], irs[0])[:n] + sg.fftconvolve(bt[:, 1], irs[1])[:n]
    drift = 10 ** ((2.0 * np.sin(2 * np.pi * np.arange(n) / FS / rng.uniform(6, 14) + rng.uniform(0, 6))) / 20)
    bdb = rng.uniform(-36, -22) if bleed_db is None else bleed_db
    leak *= drift
    leak *= 10 ** (bdb / 20) * np.sqrt(np.mean(bt ** 2)) / np.sqrt(np.mean(leak ** 2) + 1e-30)
    if not room.get("bleed", True):
        leak[:] = 0.0
    noise = pink(n, rng) if "tilt" not in room else coloured(n, room["tilt"], rr)
    noise *= 10 ** (noise_db / 20) / np.sqrt(np.mean(noise ** 2))
    hum = sum(np.sin(2 * np.pi * 50 * k * np.arange(n) / FS + rng.uniform(0, 6)) / k for k in (1, 2, 3, 5)) * 10 ** ((noise_db - 6) / 20)
    if room.get("hum", True):
        noise += hum
    if room.get("hvac_db"):
        m = sg.sosfiltfilt(sg.butter(2, 0.4, "low", fs=FS, output="sos"), rr.standard_normal(n))
        noise *= 10 ** (room["hvac_db"] * m / (3 * np.std(m) + 1e-12) / 20)
    if room.get("rumble_db") is not None:
        ru = sg.sosfilt(sg.butter(2, [4, 60], "bandpass", fs=FS, output="sos"), coloured(n, 2.0, rr, 4.0))
        noise += ru * 10 ** (room["rumble_db"] / 20) / np.sqrt(np.mean(ru ** 2))
    breath = np.zeros(n); blab = np.zeros(n, bool)
    if breaths:
        for (st, gap) in pauses:
            if gap > 0.6 * FS and rng.random() < 0.6:
                dur = int(rng.uniform(0.25, 0.55) * FS); at = st + int(rng.uniform(0.1, 0.3) * FS)
                if at + dur < st + gap - int(0.05 * FS):
                    tt = np.arange(dur) / dur
                    env = np.sin(np.pi * tt) ** 1.5
                    nz = sg.lfilter(*sg.butter(2, [300, 6000], "bandpass", fs=FS), rng.standard_normal(dur))
                    nz *= 10 ** (rng.uniform(-38, -28) / 20) / np.sqrt(np.mean(nz ** 2))
                    breath[at:at + dur] = nz * env; blab[at:at + dur] = True
    if room.get("rt60"):
        early, tail = room_ir(rr, room["rt60"], room.get("rt60_lf", 1.4), room.get("drr_db", 10.0))
        dry = voice
        voice = sg.fftconvolve(dry, early)[:n]
        noise = noise + sg.fftconvolve(dry + breath, tail)[:n]               # the room's tail counts as background
        breath = sg.fftconvolve(breath, early)[:n]
        voice *= 10 ** (-18 / 20) / np.sqrt(np.mean(voice[vlab] ** 2))
    mic = voice + leak + noise + breath
    if room.get("comp"):
        g = compress(mic, -18 + room.get("comp_thr_db", -8), room["comp"])
        g /= np.sqrt(np.mean((mic * g)[vlab] ** 2)) / np.sqrt(np.mean(mic[vlab] ** 2))   # make-up: same voice level
        voice, leak, noise, breath, mic = voice * g, leak * g, noise * g, breath * g, mic * g
    return dict(mic=mic, voice=voice, bleed=leak, noise=noise, breath=breath, beat=bt, vlab=vlab, blab=blab, bleed_db=bdb, xlab=xlab)

if __name__ == "__main__":
    d = take("slt", 6, seed=1)
    def lev(x, m=None):
        x = x if m is None else x[m]
        return 10 * np.log10(np.mean(x ** 2) + 1e-30)
    print("take: %.1f s, voice active %.0f %%" % (len(d["mic"]) / FS, 100 * d["vlab"].mean()))
    print("levels (dBFS RMS): voice on speech %.1f | bleed %.1f (set %.1f re beat) | noise %.1f | breath %.1f | beat %.1f"
          % (lev(d["voice"], d["vlab"]), lev(d["bleed"]), d["bleed_db"], lev(d["noise"]), lev(d["breath"], d["blab"]) if d["blab"].any() else -999, lev(d["beat"])))
    import wave
    def wr(p, x):
        y = (np.clip(x, -1, 1) * 32767).astype("<i2"); f = wave.open(p, "wb"); f.setnchannels(1 if x.ndim == 1 else 2); f.setsampwidth(2); f.setframerate(FS); f.writeframes(y.tobytes()); f.close()
    out = os.path.expanduser("~/axell-gate-data/corpus_demo")
    os.makedirs(out, exist_ok=True)
    wr(f"{out}/mic.wav", d["mic"] * 4); wr(f"{out}/beat.wav", d["beat"])
    print("wrote", out)
