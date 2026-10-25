# Axell Gate - design decisions

Every decision was taken on a measurement. The scripts are in `research/` and the checks in `tests/`.

## G1 - Scope: what the user gates

Asked on 2026-10-15:
- **VOCALS:** clean gaps, breath control, and removal of the beat's headphone bleed.
- **BEATS:** shaping programmed drums, and rhythmic tempo-synced gating.

Not needed: multi-mic drum bleed, or keying from another track (the side-chain carries the beat for BLEED).

## G2 - Test material with ground truth (`research/corpus.py`)

- **Takes:** CMU ARCTIC utterances strung into takes with natural pauses.
- **What is mixed in:**
  - a synthetic beat leaking through a closed-headphone model (two cups, a few reflections, ±2 dB head movement, -22
    to -36 dB);
  - pink noise and hum;
  - inhale-like breaths.
- **Scoring:** every component is known, so damage is exact.
  - Voice loss: Σ((1-g) v)².
  - Word starts: the same over the first 40 ms of each segment.
  - Background left: in pauses well away from the voice.

## G3 - The yardstick: a classic gate tuned per take

For every take, the classic level gate (100 Hz side-chain high-pass, hysteresis, look-ahead, attack, hold, release)
is searched over 324 settings. For each Axell result, the comparison is the classic setting with the least voice
damage among those that remove at least as much background. That is a perfect engineer, re-tuning for every take
(`research/bench_vocal.py`).

## G4 - Voice activity: Velvet's features plus a gate's own (`Source/VoiceActivity.h`)

- **Features:** the shared sibilance detector's frame features (YIN aperiodicity, spectral shape, singer-adapted
  bands) plus:
  - the level above a minimum-statistics floor (20 ms smoothing, 1.5 s minimum);
  - 48 ms of past context.
- **Model:** a 23 → 32 → 3 network (background / voice / breath), trained on the C++ inputs.
  - On held-out voices: 92-94 % of frames right.
  - The best per-take level threshold: 88.7 %.
- **Loud evidence:** a frame within 18 dB of the voice's running peak and 12 dB above the floor counts as voice
  whatever the network says. It removed the worst dropouts: a 102 ms hole in loud speech carried half of one take's
  voice damage.

## G5 - Decision: fixed-lag hidden Markov smoothing, opened early

- **The model:** two states, with transitions for a ~1 s mean phrase and a ~0.4 s mean pause. Emissions are the
  network's posterior over its training prior; fixed-lag smoothing uses the look-ahead.
- **Opening early beats looking far ahead.**
  - What mattered most is opening 35 ms before the first voice frame: soft starts (h, f, breathy onsets) are
    recognised late.
  - The look-ahead lag itself helps little (10 ms is enough).
- **The settings, measured in C++** (`research/bench_vocal_cpp.py`):

  | Setting | Hold | Release | Latency | Voice damage vs tuned classic | Word starts | Background removed |
  |---|---|---|---|---|---|---|
  | BEST: lag 5 frames, open 35 ms early | 60 ms | 50 ms, amplitude-linear ramps | 57 ms | 9.3 dB less (worst take 3.2 dB less) | 12.5 dB better | 37 dB |
  | FAST: lag 0, open 25 ms early | 60 ms | 50 ms | 37 ms | 5.2 dB less | 6.5 dB better | 25 dB |

- **Ramps:** amplitude-linear ramps beat dB-linear ones on voices: a dB-linear release drops a word's tail too early.
- **Rejected:** a decay-following (Wiener) release on vocals. With hold, it only kept the gate open in pauses.

## G6 - Breaths: a class and an event

- **The breath class:** it recognises about two thirds of breath frames. BREATH sets their level.
- **First fault:** in the test suite, a steady noise floor sat at breath level (-12 dB) instead of closed. Noise is
  aperiodic and broadband, like a breath.
- **Physics:** a breath rises above the room.
- **Second fault:** a broadband floor test was fooled by rumble, which wobbles the frame level by ±6 dB.
- **Rule now:** a breath also needs its own band (300 Hz - 6 kHz) to sit 6 dB above that band's slowly rising floor.
- **Result:** pauses close, and the vocal benchmark is unchanged.

## G7 - BLEED: a frequency-domain Kalman canceller (`Source/Canceller.h`)

- **NLMS with a step-size estimate (Valin 2007):** stable, but only 3-7 dB.
- **"Learn in the pauses" with oracle voice activity:** worse than nothing, because breaths and room noise are louder
  than the bleed.
- **The Kalman form** (Enzner & Vary 2006; partitioned, Kuech et al. 2014): each bin's gain is the optimal step, small
  where voice or noise dominates. 10-13 dB, the same under the voice as in the pauses.
  - Initial uncertainty is set realistically (the bleed is about 30 dB down), otherwise the first seconds learn the
    voice.
- **Efficiency:** one partition per reference is constrained per block, so 2 % CPU.
- **Safety:** the estimate is scaled by its correlation with the mic, with each block weighted by the inverse of its
  mic energy. The pauses decide, and the voice's random correlation cannot fake a mismatch. Unweighted versions lost
  4-6 dB.
- **Detectors listen to the uncleaned mic:** on the cleaned signal the network (trained with bleed) opened in 24 % of
  one take's pauses.
- **End to end** (`research/bench_bleed_cpp.py`): gaps 11-15 dB cleaner, speech 13 dB cleaner, on all three voices.

## G8 - SHAPE: decay scaling (`research/shape_sim.py`, `research/bench_shape_cpp.py`)

- **The law:** g = (e / e_peak)^alpha on the analytic (Hilbert-pair) envelope, smoothed 1 ms, monotone after the
  peak. An exponential tail e^(-t/tau) becomes e^(-(1+alpha) t / tau).
- **Against a classic release tuned to the same T40:** 10-55 dB less splatter, and the tail is as straight in dB as
  the original.
- **Faults found and fixed:**

  | Fault | Fix |
  |---|---|
  | Decay tracking began before the hit, on the noise floor | Track from the onset, after the peak |
  | The close test fired on the first sample (hold 0) | Close only once the hit is decaying |
  | The RELEASE rate limited the decay | Decay scaling follows the envelope directly |
  | Noisy hits (snare) flicker during their attack | Decay tracking starts 5 ms in |
  | Overlapping 808s: a new hit is only seen once it rises 9 dB above the old tail | The ramp ends 1.5 ms before detection (4 ms of look-ahead) |

- **Results:** TAIL 50 % gives 0.47-0.51 x the length. The first 3 ms of kick and 808 are intact to -85 / -91 dB.

## G9 - RHYTHM

- **Gain:** a pure function of host position. Raised-cosine edges are centred on the step boundaries, swing warps
  every second step, and LENGTH sets the bar (12 for triplets).
- **Latency:** 0 ms.
- **Measured:** the 50 % crossing within one sample of the grid; splatter -94 dB (0.5 ms edges) to -139 dB (10 ms).

## G10 - Engineering

- **Events:** detection runs on the undelayed signal and schedules gain events on the input timeline. The gain
  generator applies them to the delayed signal.
- **Latency:** per configuration, reported to the host through the core's handshake.
- **Exactness:** output = input x gain exactly; bypass returns the input exactly; silence stays silence.
- **Glides:** every glide lands exactly on its target (from Axell Velvet's audit).

## G11 - Known limits (1.0)

Superseded by G21.

# 2.0

## G12 - Validation on a real take, and what it showed

A real dry rap take (a large-diaphragm condenser through an interface preamp, a bare untreated room, 58 s) went
through the 1.0 gate.
- The gate was open in **80 %** of the true gaps (stretches 150 ms or more from any sound within 30 dB of the voice).
- Cause: every training room had beat bleed and mains hum. A clean booth whose room tone sits 37 dB under the voice,
  with sub-20 Hz rumble louder than the room tone above it, was outside everything the network had seen.
- Measured on the take (`research/bench_real.py`): no labels exist, so it scores the gaps, the voice energy removed
  and the word starts, against a classic gate tuned with hindsight on that same take.

## G13 - Real rooms in the test material (`research/corpus.py`, `bench_room_cpp.py`, `bench_wide.py`)

- **New room conditions:** takes without bleed, room tone white to brown, rarely hum, rumble (4-60 Hz) up to 22 dB
  under the voice, breathing air conditioning, dense rap pauses (80-500 ms), a fast compressor over the whole chain,
  and untreated-room reverberation.
- **Reverberation:** direct sound plus early reflections (up to 20 ms) count as the voice. The late tail counts as
  the room. Measured in the real take's room: RT60 0.33 s in the bass, 0.23 s above.
- **Detection path:** a 4th-order 60 Hz high-pass before the detector. The audio is not filtered.
- **Wide benchmark:** 8 conditions x 3 held-out voices, absolute time-based measures (voice lost, word starts,
  missed-voice time, false-open time in pauses). The relative measure of 1.0 (voice damage "at the classic gate's
  removal") swings by 10 dB on one 20 ms event when a condition has only 3 takes.

## G14 - Opening and closing are separate events

- **1.0 fault:** the pre-open shift was applied to closings too. The gate closed as early as it opened, so the tail
  after the last voice frame was HOLD minus the pre-open.
- **2.0:** openings run early (soft starts are recognised late), closings land after the hold at their own frame. A
  decision that overtakes pending events replaces them, so a pause shorter than the pre-open never closes.
- **Measured:**

  | Speed | Setting | Latency | Word starts vs tuned classic (step-1 model) |
  |---|---|---|---|
  | BEST | open 45 ms early, HOLD 30 ms | 67 ms | -16.3 dB (1.0: -12.5) |
  | FAST | open 30 ms early | 42 ms | -6.6 dB (1.0: -6.5) |

  The final detector's numbers are in G21.

- **HOLD default:** 30 ms. 1.0 projects and presets are converted on load (HOLD minus 35 ms BEST, 25 ms FAST), so
  every saved tail is kept.

## G15 - BLEED: learning persists; two canceller faults fixed

- **Persistence:** what the canceller learned is saved with the project (34 KB at 48 kHz) and restored at the same
  sample rate:
  - the leak path as impulse responses;
  - its uncertainty per partition;
  - the safety statistics.
  The audio thread publishes a copy twice a second through a seqlock; a restore arrives through an atomic handoff.
  `reset()` and a re-prepare at the same rate keep the path (1.0 forgot it).
- **Measured:** a reopened project removes 36 dB of bleed in the first phrase (a fresh one: 10 dB); through the
  plugin, 16.7 dB in the first second (fresh: 1.4 dB).
- **Fault 1:** a digitally silent mic with the beat still playing (the take has ended, the track is muted).
  - The canceller subtracted its estimate from silence and unlearned the path.
  - Fix: such blocks are skipped. Silence stays exactly silence (tested).
- **Fault 2:** the safety evidence was weighted by 1/mic energy. One nearly silent block under a loud estimate (a
  take fading out) dropped the weight to 0.02 for seconds. Fix: 1/max(mic, estimate) energy, so no block counts for
  more than a normal pause.

## G16 - The BLEED meter

- **GAPS:** how much quieter the mic is where the bleed is plain: 2 ms stretches whose fitted estimate carries 90 %
  of the mic's energy.
- **Rejected definitions:**
  - Gaps by the gate's decision read 1-3 dB on a dense rap take whose quiet stretches really got 8-11 dB quieter. The
    decision runs behind the audio, and dense rap rarely gives 128 ms of decided gap.
  - Lower selection thresholds let word edges in, which caps the reading.
- **Measured:** conservative by design: 22 dB shown for 30 dB removed (synthetic), 7 dB for 8-11 dB (real take). A
  test checks it never reads more than the true removal.
- **FIT** (the safety weight) and **BEAT** (side-chain level) complete it.

## G17 - SHAPE -> MIDI, RHYTHM step levels, LEVEL key filters

- **MIDI:** a note per hit at the hit as heard (the latency-compensated output), with velocity from the hit's peak
  over its first 3 ms (-48 dBFS -> 1 .. 0 dBFS -> 127).
  - The velocity is written into the hit's own queued event before the output reaches it (SHAPE latency 4 ms + the
    look-ahead covers the 3 ms).
  - A note ends with the number it started with, and reset() flushes a sounding note.
  - Measured: one note per kick, within 3 ms of the hit, at 3 rates; 20 dB quieter hits come out 53 velocity steps
    lower.
- **RHYTHM:** a level for each of the 16 steps. The gain at the step centres is exact (error < 1e-4).
- **VOCAL LEVEL:** KEY HP (default 80 Hz = 1.0's fixed filter), KEY LP, KEY LISTEN.
- **Parameters:** 20 appended (1.0's 35 IDs, order and hints unchanged). Presets: 4 appended (1.0 program numbers
  kept).

## G18 - A model of the room (`Source/RoomModel.h`)

- **The problem:** in an untreated room every word rings on. The tail is periodic (it is the voice), low in frequency
  and loud next to the room tone. On every frame feature it looks like soft voice. Measured on a bare-room take
  generated for the purpose: 20 ms after a word, the network still gave pV 0.88; the gate closed ~120 ms after the
  word.
- **The idea:** after the excitation stops, each band can only fall at the room's own rate. The room is learned and
  its tail predicted, as in dereverberation's late-reverberation estimator (Lebart et al. 2001; Habets 2009). Here
  the prediction is used as detection evidence.
  1. **RT60, blind, per band (4 bands):** the late phase of free decays. The level had already been falling for
     40 ms, and the next 40 ms fit a line within 1.5 dB RMS. The room's rate is the 60th percentile. A word can
     fade more slowly than its room, never faster; the direct sound's own drop is excluded by the "already falling"
     rule. Measured: within ~15 % on synthetic rooms of 0.25 and 0.5 s. On the real take: 0.26 / 0.23 s
     (0.33 / 0.23 s by hand).
  2. **Prediction:** the energy 40 ms earlier, decayed at the room's rate, with 3 dB of margin. What it cannot
     explain is new excitation.
  3. **Network inputs:**
     - the unexplained share;
     - its level over the band floor (a slowly rising minimum, unlike the 1.5 s floor that dense rap fools);
     - the RT60;
     - the fall re the room's rate.
- **The room veto:** a frame-level veto failed. Inside words, single frames between syllables look like room decay,
  and voice lost went from -65.4 to -49.7 dB. The veto that works is a run. Once the room has explained the signal
  without a break for 30 ms (smoothed over ~10 ms), the voice evidence, the network's and the level's, is withdrawn.
  - Measured in a bare RT60 0.5 s room: background left -6.9 -> -17.1 dB, voice unchanged.
  - A per-frame reset of the run (instead of smoothing) almost never let it fire.

## G19 - 27 voices, and labels that agree (`research/vctk_fetch.py`, `corpus.vctk_labels`)

- **Why:** with three voices, each held-out test model had heard one or two others. The only woman (slt) failed in
  reverberant rooms with a model that had never heard a woman.
- **Data:** 24 speakers of the CSTR VCTK Corpus (CC BY 4.0): 12 women, 12 men, 11 accents, 40 sentences each. They
  were taken from the 11.7 GB archive with HTTP range requests (the zip index once, then each recording as one
  exact range; ~15 minutes).
- **Labels from level:** VCTK has no phone labels, and its floor is not silence (~35 dB under the voice, wobbling).
  - The labeller was calibrated against CMU ARCTIC's phone labels, with a VCTK-like floor added (180 utterances):
    floor + 6 dB, gaps under 150 ms filled, onsets 20 ms early. 96.0 % agreement.
  - The first, untested version missed 21 % of the voice. It taught the network to miss voice: missed time
    1.0 -> 2.0 %, and no threshold could undo it.
- **Don't-care frames:** the filled gaps hold only bleed and room in a take. Labelled voice, they taught "bleed
  between words = voice" (pauses in a bleed take: -46 -> -8 dB). Labelled pause, they would fight the phone-label
  convention. They are left out of training (16.7 % of the frames).
- **Network:** 27 -> 64 -> 3. The training prior (77 % voice) is written into the model and used by the decision.

## G20 - Pauses have a duration (`FixedLagHmm`)

- **The fault:** a word's final release ("...t") after its 40-60 ms closure. The closure took the decision to
  "pause"; the release (24 dB over the floor, unexplained by the room, pV 0.6-0.9) did not bring it back. Two
  takes lost ~90 % of their voice damage there.
- **Model:** the pause is two hidden states.
  - "Just paused": resumes at 3 % per 2 ms frame and drifts into a true pause at 1.2 % per frame, ~50 ms.
  - "True pause": resumes at 0.5 % per frame, as in 1.0.

  This is a phase-type approximation of the pause-duration distribution, i.e. a hidden semi-Markov model, still
  exact under fixed-lag smoothing.
- **Measured:** the same evidence gives 3.7x the voice probability 50 ms into a pause as after 600 ms. Main
  benchmark: -4.7 -> -7.9 dB vs the tuned classic, and the worst take +3.9 -> -1.7 dB. The held-out wide
  benchmark is unchanged.

## G21 - Results, and what is left

- **Faults found and fixed:**
  - **Digital silence before a take** (any DAW, before the vocal item) set the room model's and the breath
    detector's floors to -200 dB. They rose 3 dB/s, so the gate opened for over a minute (the breath floor's flaw
    dates from 1.0). The independent audit found it: real-room pauses closed in 8 % (now 100 %). Floors now ignore
    digital silence, and a test with 2 s of silence before the take is in the suite.
  - **A synthetic test tail** (a low-passed signed voice times noise) was really continuous noise. It is replaced
    by a diffuse tail (noise with the voice's energy envelope decayed at the room's rate).
- **Held-out wide benchmark** (`bench_wide.py`, never tuned on; 8 conditions x 3 voices, pooled):

  | | voice lost | word starts | missed voice | false opens | background left |
  |---|---|---|---|---|---|
  | 2.0, 3 voices, no room model | -49.5 dB | -44.2 dB | 1.01 % | 5.35 % | -24.6 dB |
  | **2.0 final, BEST** | **-63.2 dB** | **-44.2 dB** | 1.09 % | **2.82 %** | **-34.7 dB** |
  | 2.0 final, FAST | -53.9 dB | -32.3 dB | 1.51 % | 2.73 % | -32.4 dB |

- **Against the classic gate re-tuned for every take** (a perfect engineer):

  | Test | Result |
  |---|---|
  | Original takes | BEST 7.7 dB less voice damage (every take better; worst -1.7 dB), word starts 15.6 dB better, background -32 dB. FAST: 7.3 dB less, worst -2.9 dB. |
  | Bare untreated room | 11.8 dB less |
  | Air conditioning | 4.1 dB less |
  | Booth with rumble | 0.8 dB less. Its whole-take worst (+2.2 dB) is the corpus recordings' own sounds after the labelled words; on the words alone the smart gate is better. |
  | Loud headphones + 8:1 compressor | 3.1 dB less; one take is 3.1 dB behind (see the limits) |

- **The real take** (bare room): open in 5 % of the true gaps (1.0: 80 %), voice energy removed -37.8 dB, word
  starts -46 dB.
- **BLEED** (gate + canceller, pauses): -38.7 to -49.3 dB.
- **Known limits:**
  - **Very live rooms (RT60 0.5 s and more):** the tail after a word is released ~0.15-0.25 s after it.
  - **Heavy compression plus loud bleed:** the background sits within ~20 dB of the voice. Inside a word, 60 ms of
    voice only 5 dB above it can close the gate (one take, -40 dB of voice energy). Route the beat to the side-chain
    (BLEED) there.
  - **Training material:** read speech by 27 voices. Rap delivery, singing and screams are covered by the real
    take and the dense/compressed conditions, not by labelled rap data; no free labelled corpus exists.
  - **Breaths** are synthetic in training (78 % recognised on held-out voices).

## G22 - The artist's setup: studied, 2.0's network kept (`research/gate_setup.py`, `research/dense_labels.py`)

- **Labels from the artist's own dense takes do not hold up.** The VCTK labeller's threshold lands mid-voice on dense
  rap in a 0.3 s room (22 % of the voice missed on matched synthetic takes); a De-Reverb + floor labeller reached
  88-91 % on held-out voices, under G19's 96 % bar. Not used for training.
- **Instead, the setup with true labels:** corpus voices in the measured room (mids 0.13-0.24 s, the bass 1.3-5x
  longer), the measured floor (1/f^1.3-1.7, 22-40 dB under the voice), the Volt's 76 on half the takes, and the artist's
  real beat (a with-beat take minus its acapella) as headphone bleed; 108 takes added to 2.0's data (~26 % of the
  takes, 1.94 M frames).
- **Held-out benchmarks** (2.0 -> retrained): the setup conditions improved (voice -41.0 -> -43.1 dB, missed
  2.22 -> 1.87 %, worst take -16.2 -> -23.5 dB, false opens 4.62 -> 2.13 %; one condition's 60 % false opens are
  0.01 s of 0.02 s of pause, i.e. nothing); the wide benchmark held (voice -63.2 -> -63.3, starts -44.2 -> -45.0,
  missed 1.09 -> 1.12 %, false opens 2.82 -> 2.14 %).
- **The artist's 10 real takes** (no labels): closed slightly more in the quietest frames (open 29.6 -> 26.2 %) but
  lost slightly more voice on the louder half (-28.0 -> -27.5 dB, worst -17.6 -> -16.9 dB): no real-world gain.
  **Not shipped.** The gain there is in the settings (the Dojo's Home Room preset: De-Reverb 95 % before the gate).
