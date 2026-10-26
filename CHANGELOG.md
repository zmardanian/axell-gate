# Changelog

## 3.0.0 - 2026-10-26
- **VOCAL detection: two networks.** 2.0's network decides wherever it is sure of a voice; on doubtful frames a
  second network, trained on 111 voices (108 VCTK voices with forced-aligned labels, from Axell Velvet 3.0), must
  hear a voice too. On voices neither network trained on, 12.4 dB less voice lost and a third of the missed voice; on
  the artist's dry setup, false openings 1.88 -> 0.22 %; background 3.3 dB lower on the held-out wide benchmark. On
  the artist's own takes the voice is kept as in 2.0 (DECISIONS G23).
- **The suite's shared voice detector.** Gate's detector now lives in axell-core, bit-identical; Axell Duo
  Compressor 3.0's VOICE HOLD uses the same one.
- **Axell's Dojo look**: the family emblem, a brush-written title with Gate's seal (門, "gate") and Gate's own
  indigo ground.
- Verified: DSP 278 checks (CPU: VOCAL 1.9 %, + BLEED 3.9 % of one core), plugin 104, ASan + UBSan clean, independent
  audit 42/42, pluginval strictness 10, Steinberg's VST3 validator 47/47.

## 2.0.0 - 2026-10-16
Validated on a real dry rap take from a bare, untreated room, and rebuilt where it fell short.

- **VOCAL detection:**
  - **Hears the room:** a room model learns the room's reverb time by itself and tells the voice from the room
    ringing on after it (new).
  - **More voices:** trained on 27 voices (24 added: women and men, 11 accents) and on real rooms: no bleed, room
    tone of any colour, rumble, air conditioning, reverberation, dense rap pauses, compression.
  - **A model of pause length:** a word's final consonant after its closure is no longer cut.
  - **A 60 Hz high-pass in front of the detector:** rumble no longer opens it.
  - **Measured on 24 takes it was never tuned on:** 13.7 dB less voice removed, half the false openings, 10 dB more
    background removed than with 3 voices.
  - **On the real take:** open in 5 % of the true gaps (1.0: 80 %).
- **Timing:** openings run early, closings land after the hold at their own time (1.0 closed as early as it
  opened).
  - BEST opens 45 ms early (67 ms latency), FAST 30 ms (42 ms).
  - HOLD now means the time after the last voice frame (default 30 ms).
- **BLEED:**
  - What it learned is saved with the project, and a reopened project cancels from the first second.
  - A meter (GAPS / FIT / BEAT) and a status line.
  - Fixes: a silent mic with the beat still playing no longer unlearns the path or adds bleed to silence, and one
    quiet block can no longer collapse the safety weight.
- **SHAPE -> MIDI:** a note for every detected hit (HIT NOTE; velocity from the hit), exactly at the hit as heard.
- **RHYTHM:** a level for each of the 16 steps.
- **VOCAL LEVEL:** KEY HP, KEY LP, KEY LISTEN.
- **GUI:**
  - the BLEED meter;
  - breath markers and hit markers in the history;
  - step-level bars;
  - the KEY section and HIT NOTE.
- **Presets:** 4 new (Kick Trigger (MIDI), Ducking Pump, Accent Chop, Live Tracking - Boomy Room).
- **Fix:** digital silence before a take (any DAW, before the vocal item) held the gate open for up to a minute.
- **Compatibility:** 1.0 projects and user presets load. HOLD is converted to keep each saved tail, new parameters
  start at their defaults, and the 1.0 parameter IDs and program numbers are unchanged.

## 1.0.0 - 2026-10-16
First release of Axell Gate, the gate of the Axell suite.

- **VOCAL:**
  - Smart voice detection: a voice-activity network, fixed-lag hidden Markov decision and loud-level evidence.
  - BEST / FAST / LIVE speeds.
  - Sensitivity, and breath level from removed to untouched.
  - BLEED REMOVAL: a frequency-domain Kalman canceller with the beat on the side-chain, with FREEZE.
- **SHAPE:** decay scaling (TAIL), onset-exact opening with look-ahead, threshold, hit sensitivity, attack, hold,
  release.
- **RHYTHM:** 16-step host-synced patterns, length 2-16, rates 1/4-1/32 and triplets, swing, raised-cosine edges.
- **Presets and workflow:** 21 factory presets, user presets, A/B.
- **Engineering:** latency reported exactly through the handshake; host bypass exact and latency-aligned.
