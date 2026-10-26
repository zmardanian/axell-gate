# Axell Gate (VST3, Linux)

[![CI](https://github.com/zmardanian/axell-gate/actions/workflows/ci.yml/badge.svg)](https://github.com/zmardanian/axell-gate/actions/workflows/ci.yml)

The gate of the Axell suite, built for vocals and beats:
- **VOCAL** opens on your voice, not on a level. It keeps every word start, sets breaths to the level you choose,
  hears your room (it tells the voice from the room ringing on after it) and, with the beat on its side-chain,
  removes the beat's headphone bleed in the gaps *and* under the voice.
- **SHAPE** shortens drum tails without clicks and without the kink a release puts in a decay.
- **RHYTHM** chops on the host's grid with click-free edges.

> **Part of Axell's Dojo** - the Axell Signature suite for rap vocals, beats and masters on Linux:
> [Penta EQ](https://github.com/zmardanian/axell-penta-eq) · [Duo Compressor](https://github.com/zmardanian/axell-duo-compressor) ·
> [Gate](https://github.com/zmardanian/axell-gate) · [De-Reverb](https://github.com/zmardanian/axell-dereverb) ·
> [Velvet](https://github.com/zmardanian/axell-velvet) · [Master](https://github.com/zmardanian/axell-master), installed,
> updated and tuned to your voice by the [Axell Signature Dojo](https://github.com/zmardanian/axell-signature-dojo).
> Since 3.0 the plugins share one core library (`external/axell-core`): a breakthrough in one plugin becomes a
> feature of the next.

![Axell Gate](docs/screenshots/gate-vocal.png)

## What's new in 3.0

- **Two voice networks**: 2.0's network decides wherever it is sure of a voice; on doubtful frames a second network,
  trained on 111 voices (108 VCTK voices with forced-aligned labels, from Axell Velvet 3.0's research), must hear a
  voice too. On voices neither network trained on: **12.4 dB less voice lost** and a third of the missed voice; on the
  artist's dry setup, false openings 1.88 -> 0.22 %; background 3.3 dB lower on the held-out wide benchmark. The
  artist's own takes keep 2.0's voice (DECISIONS G23).
- **The suite's shared voice detector**: Gate's detector now lives in the shared core, bit-identical; Axell Duo
  Compressor 3.0 and Axell Master 3.0 use it for VOICE HOLD.
- **Axell's Dojo look**: the family emblem, a brush-written title with Gate's seal (門, "gate"), an indigo ground.

More screens: [BLEED meter](docs/screenshots/gate-bleed.png), [LEVEL key](docs/screenshots/gate-level-key.png), [SHAPE with MIDI](docs/screenshots/gate-shape-midi.png), [RHYTHM step levels](docs/screenshots/gate-rhythm-levels.png).

## What it does

**VOCAL**
- **SMART** detection: no threshold to set; the same setting works on a whisper and a shout. Four parts:
  - **Two voice-activity networks** (3.0): 2.0's, trained on 27 voices with headphone bleed, room noise, rumble,
    untreated-room reverberation, compression and breaths, decides wherever it is sure; on doubtful frames a second
    network, trained on 111 voices, must agree. Far less voice lost on voices it never heard, fewer false openings.
  - **Room model:** learns your room's reverb time by itself, so the tail after a word is not taken for voice.
  - **Pause model:** knows that a 40 ms closure inside a word is not a pause.
  - **Decision:** a hidden Markov model over a short look-ahead.
- **SPEED**: BEST (67 ms latency), FAST (42 ms), LIVE (0 ms, a classic level gate for monitoring while you record).
- **SENSITIVITY**: opens more or less readily.
- **BREATH**: how loud breaths stay (removed .. untouched).
- **BLEED REMOVAL**: route the beat into the side-chain input. A frequency-domain Kalman filter learns how the beat
  leaks from your headphones into the mic and subtracts it. It never needs a threshold and never adds bleed (a safety
  check scales it down when the estimate does not fit). What it learned is saved with the project. The meter shows
  how much quieter the gaps are (GAPS), how well the learned leak fits (FIT) and the beat's level. FREEZE keeps what
  it learned.
- **LEVEL** detection (and LIVE): KEY HP / KEY LP filter what the level detector hears; KEY LISTEN lets you hear it.

**SHAPE** (programmed drums, samples, 808s)
- **TAIL**: decay scaling. The gain follows each hit's own envelope, g = (e / e_peak)^alpha, so an exponential tail
  stays exponential, just faster. TAIL 50 % = half as long, same shape, no click. The opening ramp ends exactly at the
  onset (look-ahead), even on a hit that lands on the previous one's ringing tail.
- **THRESHOLD**, **HIT SENS**, **ATTACK**, **HOLD**, **RELEASE** (the final close), **RANGE**.
- **MIDI out**: a note for every hit (HIT NOTE; velocity from the hit), exactly where you hear the hit. Layer a
  sample, or drive a synth from the drums.

**RHYTHM**
- 16 steps, each with its own level (LENGTH 2-16; 12 for triplet bars), RATE 1/4 to 1/32 and triplets, SWING, EDGE
  (raised-cosine edges centred on the grid), RANGE.
- It follows the host's tempo and position; stopped, it free-runs.

There are 25 factory presets (vocals, drums, rhythm), user presets and A/B. Projects and presets from 1.0 load.

## Measured quality

| | |
|---|---|
| VOCAL vs a classic level gate whose threshold, look-ahead, hold and release were searched for every take (a perfect engineer), at equal background removal | BEST: 7.7 dB less damage to the voice on average (and less in every take), 15.6 dB less to word starts. FAST: 7.3 / 7.4 dB less. In a bare untreated room: 11.8 dB less. One fixed setting, no tuning. |
| VOCAL on 24 takes in 8 recording conditions, never used for tuning (voices held out) | 3.0: voice energy removed -62 dB; open in 2.4 % of the pause time; background left -38 dB. (2.0: -63 dB, 2.8 %, -35 dB; 1.0's three voices, no room model: -50 dB, 5.4 %, -25 dB.) |
| VOCAL on 6 voices no network trained on (8 conditions) | 3.0: voice energy removed -67 dB, missed voice 0.12 % (2.0: -55 dB, 0.33 %) |
| The artist's own setup (their room, mic, preamp, real beat as bleed) and 10 real takes | 3.0: false openings 4.4 % (dry: 0.2 %), open in 24 % of the gaps of the real takes; the voice kept as in 2.0 |
| A real dry rap take from a bare room | open in 5 % of the true gaps (1.0: 80 %), word starts intact (-46 dB) |
| BLEED (beat on the side-chain) | gaps 11-15 dB cleaner, speech 13 dB cleaner than the gate alone; 10-13 dB of bleed removed under the voice; a reopened project cancels from the first second (36 dB vs 10 dB) |
| SHAPE MIDI | one note per hit, within 3 ms of the hit, velocity following the hit's level |
| SHAPE vs a classic release tuned to the same tail length | 10-55 dB less click (splatter) on kick, 808, tom, snare, clap; the tail's shape stays natural; TAIL 50 % = 0.47-0.51 x the length |
| RHYTHM | edges centred on the grid within a sample; splatter -94 dB (0.5 ms edges) to -139 dB (10 ms) |
| Silence in | exactly silence out (all modes, 44.1 / 48 / 96 kHz) |
| Host bypass | exactly the input, delayed by the reported latency |
| CPU (48 kHz stereo, one core) | VOCAL 1.9 %, with BLEED 3.9 %, SHAPE 0.16 %, RHYTHM 0.13 % |
| Real-time safety | no allocation, lock or I/O in the audio path (allocation counter in the test suite) |

All of these come from:
- the research benchmarks in `research/` (CMU ARCTIC takes with known bleed, noise and breaths; held-out voices);
- the automated suites in `tests/`: 278 DSP checks at three sample rates and 104 plugin checks;
- an independent black-box audit through a VST3 host (`tests/independent/gate_audit.py`, 42 checks);
- AddressSanitizer + UBSan;
- pluginval at strictness 10;
- Steinberg's VST3 validator (47/47).

The reasoning behind every design choice is in [docs/DECISIONS.md](docs/DECISIONS.md).

**Known limits** (details in DECISIONS G21):
- In a very live room (RT60 0.5 s and more), the room's tail after a word is released 0.15-0.25 s after the word.
- With heavy compression *and* loud headphone bleed, the background can sit within ~20 dB of the voice. Very quiet
  parts of a word may then dip; route the beat to the side-chain (BLEED) there.

## Engineering progress

| Version | Date | Engineering | Measured |
|---|---|---|---|
| **1.0.0** | 2026-10-16 | VOCAL: voice-activity network (3 voices) + fixed-lag HMM + loud evidence; BLEED (frequency-domain Kalman canceller with the beat on the side-chain); SHAPE (decay scaling with look-ahead); RHYTHM (16 steps, host sync); 21 presets | on a real dry rap take from a bare room (measured during 2.0's validation): open in 80 % of its true gaps - the reason for 2.0 |
| **2.0.0** | 2026-10-16 | Validated on a real dry rap take and rebuilt: a room model (the room's tail is not voice), 27 training voices + real-room conditions, a pause-length model, a 60 Hz detector high-pass, early openings / timed closings, BLEED saved with the project, SHAPE MIDI out, step levels, KEY filters | 24 held-out takes: 13.7 dB less voice removed, half the false openings, 10 dB more background removed; the real take: open in 5 % of its gaps (1.0: 80 %); vs the perfect-engineer classic gate: 7.7 dB less voice damage |
| **3.0.0** | 2026-10-26 | A second network trained on 111 voices (forced-aligned labels from Axell Velvet's research), consulted where 2.0's is unsure; the detector moved into the shared core (bit-identical) for Duo's and Master's VOICE HOLD; Axell's Dojo look | Unseen voices: voice lost -54.8 -> -67.2 dB, missed 0.33 -> 0.12 %; dry setup false opens 1.88 -> 0.22 %; the artist's takes as in 2.0; 278 DSP + 104 plugin checks, ASan clean, audit 42/42, pluginval L10, validator 47/47 |

Every design decision with its measurement: [docs/DECISIONS.md](docs/DECISIONS.md); all changes: [CHANGELOG.md](CHANGELOG.md).

## Install (Linux)

Download `AxellGate-3.0.0-linux-x86_64.tar.gz` from the
[latest release](https://github.com/zmardanian/axell-gate/releases/latest) (optional: `sha256sum -c SHA256SUMS.txt`), then:

    tar xzf AxellGate-3.0.0-linux-x86_64.tar.gz
    cd AxellGate-3.0.0-linux-x86_64
    ./install.sh

Or let the [Axell Signature Dojo](https://github.com/zmardanian/axell-signature-dojo) install and update it.

Then in REAPER: Options > Preferences > Plug-ins > VST > Re-scan. For BLEED REMOVAL, send the beat's track to the
vocal track on channels 3/4 and, in the plugin's pin connector, connect inputs 3/4 to the side-chain.

## Build (Bazzite / any Linux, inside a distrobox)

    bash scripts/setup-toolbox.sh
    bash scripts/build.sh
    bash scripts/test.sh               # DSP + plugin suites (--quick, --asan)
    bash scripts/pluginval.sh
    bash scripts/install.sh

The DSP lives in JUCE-free headers (`Source/Engine.h`, `VoiceActivity.h`, `Canceller.h` and the shared Axell core in
`external/axell-core` or `../axell-core`, which holds the voice detector and the room model). The voice models are
generated by `research/train_gate.py` from the features of the shipping C++ code (`tools/GateFeatures.cpp`); the 3.0
benchmarks are `research/bench3_gate.py` and `research/calib3_gate.py`.

## Contact

Axell Yobag - axellpetre@protonmail.com. Issues on GitHub are welcome too.

Licence: GNU AGPLv3 (`LICENSE`); third-party components and the CMU ARCTIC and VCTK notices (training data) in
`THIRD_PARTY_NOTICES.md`. No code, data or graphics of any commercial product are used.
