#pragma once
// Axell Gate - the whole signal chain, framework independent (no JUCE): the plugin wrapper only maps parameters.
//
//   in -> sanitize -> [dry ring: bypass, aligned to the full latency] -> [BLEED canceller, side-chain = the beat]
//      -> detection on the undelayed signal, events scheduled on the input timeline:
//           VOCAL  SMART: voice-activity network -> fixed-lag hidden Markov smoothing -> hold -> voice / breath / closed
//                  LEVEL: classic side-chain peak detector with hysteresis and hold (zero latency in LIVE)
//           SHAPE  transient onsets (sample accurate) -> decay scaling g = (e / e_peak)^alpha of each hit's own envelope
//           RHYTHM host-synced step pattern with raised-cosine edges and swing
//      -> gain generator (ramps in dB, corners rounded) applied to the delayed signal -> output gain -> bypass -> out
//
// Real-time safety: prepare() allocates everything; process() never allocates, locks or throws.
#include <axell/dsp/Filters.h>
#include <axell/dsp/History.h>
#include <axell/dsp/LatencyHandshake.h>
#include <axell/dsp/Math.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "Canceller.h"
#include "VoiceActivity.h"

#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
 #include <xmmintrin.h>
 #define AXG_HAS_SSE 1
#endif

namespace axg
{
using namespace axell;

enum Mode : int { kVocal = 0, kShape = 1, kRhythm = 2 };
enum Detect : int { kSmart = 0, kLevel = 1 };
enum Speed : int { kLive = 0, kFast = 1, kBest = 2 };

struct Params
{
    int mode = kVocal;
    // VOCAL
    int detect = kSmart;
    int speed = kBest;          // LIVE (0 ms, level detection), FAST (~35 ms), BEST (~75 ms)
    float thresholdDb = -45.0f; // LEVEL detection; SHAPE: the level a hit must reach
    float sensitivity = 0.0f;   // SMART: -100 (strict) .. +100 (open more readily)
    float breathDb = -12.0f;    // level of breaths: -80 = removed, 0 = untouched
    bool bleed = false;         // remove the beat's headphone bleed (the beat on the side-chain)
    bool bleedFreeze = false;   // keep the learned bleed path, stop learning
    // common
    float rangeDb = -60.0f;     // closed gain
    float attackMs = 2.0f;      // opening ramp (ends at the onset)
    float holdMs = 30.0f;
    bool pitchHold = true;      // 4.0 LEVEL detection: a pitched voice under the threshold keeps the gate open
                                // (new instances and presets; states saved before 4.0 load with it off)
    float releaseMs = 50.0f;
    // SHAPE
    float tailPct = 100.0f;     // 100 natural, 50 twice as fast, 10 ten times
    float sensDb = 9.0f;        // onset: rise above the slow envelope
    int midiNote = 36;          // 2.0: the MIDI note sent for every hit (velocity from the hit's peak)
    // RHYTHM
    uint32_t pattern = 0x5555;  // 16 steps, bit k = step k open
    int steps = 16;             // pattern length 2..16 (12 for triplet bars)
    int rate = 2;               // 0 1/4, 1 1/8, 2 1/16, 3 1/32, 4 1/8T, 5 1/16T
    float swingPct = 0.0f;
    float edgeMs = 3.0f;
    std::array<float, 16> stepLevel { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 }; // 2.0: open level of each step
    // VOCAL LEVEL key (2.0; 1.0 was a fixed 80 Hz high-pass)
    float keyHpHz = 80.0f;      // 20 .. 2000
    float keyLpHz = 20000.0f;   // 1000 .. 20000 (20000: off)
    bool keyListen = false;     // hear the key instead of the output
    // global
    float outputDb = 0.0f;
    bool bypass = false;
    // research knobs (fixed in the plugin)
    float preOpenMs = -1.0f;    // VOCAL SMART: how early the gate opens before the voice frame (-1: by speed)
    float roomVeto = 1.0f;      // VOCAL SMART: the room veto (1 = full)
    float roomRunMs = 30.0f;    // VOCAL SMART: how long the room must explain the signal before the veto
    double resumePerFrame = 0.03;  // VOCAL SMART: "just paused" -> voice, per 2 ms frame (0 with toLong 1: 1.0's HMM)
    double toLongPerFrame = 0.012; // VOCAL SMART: "just paused" -> "true pause", per 2 ms frame (~50 ms)
    float closeEarlyMs = -1.0f; // VOCAL SMART: how early a close lands before its decision frame (-1: 0 ms)
    int rampLaw = 1;            // 0 dB-linear ramps, 1 amplitude-linear ramps (measured better on vocals, DECISIONS)
    int lagFrames = -1;         // VOCAL SMART: hidden-Markov look-ahead in 2 ms frames (-1: by speed)
};

struct Transport
{
    bool valid = false, playing = false;
    double ppq = 0.0;           // quarter notes at the first sample of the block
    double bpm = 120.0;
};

struct MidiEvent                // SHAPE: one note per hit (velocity 0 = note off)
{
    int offset = 0;             // sample in the processed block
    uint8_t note = 36, velocity = 0;
};

struct DisplayFrame             // one per 2 ms
{
    float inDb = -120.0f, outDb = -120.0f, gainDb = 0.0f, pVoice = 0.0f, pBreath = 0.0f, floorDb = -120.0f;
    uint8_t state = 0;          // 2.0, as heard: 0 closed, 1 breath level, 2 open (VOCAL)
    bool hit = false;           // 2.0: SHAPE - a hit started in this frame
};

struct ScopedFtz
{
#if AXG_HAS_SSE
    ScopedFtz() noexcept : saved (_mm_getcsr()) { _mm_setcsr (saved | 0x8040u); }
    ~ScopedFtz() { _mm_setcsr (saved); }
    unsigned int saved;
#endif
};

//==============================================================================
// hidden Markov model of voice and pause, fixed-lag smoothing: P(voice at t - lag | evidence up to t).
// 2.0: the pause is two hidden states - "just paused" (inside a phrase: a stop's closure, a breath between words; the
// voice is likely to resume within tens of ms) and "true pause" (it rarely resumes) - so the chance of the voice
// resuming depends on how long the pause has lasted (a phase-type approximation of the pause-duration distribution,
// i.e. a hidden semi-Markov model). A word's final release after its closure then reopens the gate on the evidence
// it gives; in a long pause the same evidence is not enough. States: 0 true pause, 1 voice, 2 just paused.
class FixedLagHmm
{
public:
    static constexpr int kMaxLag = 64;
    static constexpr int kS = 3;
    void setup (int lagFrames, double stayVoice, double stayBack, double priorVoice, double resume = 0.0, double toLong = 1.0) noexcept
    {
        lag = std::clamp (lagFrames, 0, kMaxLag - 1);
        for (auto& r : T)
            r.fill (0.0);
        T[1][1] = stayVoice;                   // voice -> voice
        T[1][2] = 1.0 - stayVoice;             // voice -> just paused
        T[2][1] = resume;                      // just paused -> voice
        T[2][0] = toLong;                      // just paused -> true pause
        T[2][2] = std::max (0.0, 1.0 - resume - toLong);
        T[0][0] = stayBack;                    // true pause -> true pause
        T[0][1] = 1.0 - stayBack;              // true pause -> voice
        prior = priorVoice;
    }
    void reset() noexcept
    {
        a = { 0.5, 0.5, 0.0 };
        count = 0;
        pos = 0;
    }
    int lagFrames() const noexcept { return lag; }
    // push the evidence P(voice) of the newest frame; returns false until `lag` frames exist; post = P(voice) of the
    // frame `lag` frames back
    bool push (double pv, double& post) noexcept
    {
        pv = std::clamp (pv, 1.0e-4, 1.0 - 1.0e-4);
        const double lb = (1.0 - pv) / (1.0 - prior), lv = pv / prior;
        const std::array<double, kS> lik { lb, lv, lb };
        std::array<double, kS> na {};
        double s = 0.0;
        for (int j = 0; j < kS; ++j)
        {
            double v = 0.0;
            for (int i = 0; i < kS; ++i)
                v += a[(size_t) i] * T[(size_t) i][(size_t) j];
            na[(size_t) j] = v * lik[(size_t) j];
            s += na[(size_t) j];
        }
        for (int j = 0; j < kS; ++j)
            a[(size_t) j] = na[(size_t) j] / s;
        alpha[(size_t) pos] = a;
        likRing[(size_t) pos] = lik;
        const int cur = pos;
        pos = (pos + 1) % kMaxLag;
        ++count;
        if (count <= lag)
            return false;
        // backward pass over the lag frames
        std::array<double, kS> b { 1.0, 1.0, 1.0 };
        for (int u = 0; u < lag; ++u)
        {
            const auto& l = likRing[(size_t) ((cur - u + kMaxLag) % kMaxLag)];
            std::array<double, kS> nb {};
            double sb = 0.0;
            for (int i = 0; i < kS; ++i)
            {
                double v = 0.0;
                for (int j = 0; j < kS; ++j)
                    v += T[(size_t) i][(size_t) j] * l[(size_t) j] * b[(size_t) j];
                nb[(size_t) i] = v;
                sb += v;
            }
            for (int i = 0; i < kS; ++i)
                b[(size_t) i] = nb[(size_t) i] / sb;
        }
        const auto& at = alpha[(size_t) ((cur - lag + kMaxLag) % kMaxLag)];
        double q = 0.0;
        for (int i = 0; i < kS; ++i)
            q += at[(size_t) i] * b[(size_t) i];
        post = at[1] * b[1] / q;
        return true;
    }

private:
    std::array<std::array<double, kS>, kS> T {};
    double prior = 0.73;
    std::array<double, kS> a { 0.5, 0.5, 0.0 };
    std::array<std::array<double, kS>, kMaxLag> alpha {}, likRing {};
    int lag = 0, count = 0, pos = 0;
};

//==============================================================================
// 4.0 PITCH HOLD (VOCAL, LEVEL detection - the zero-latency gate): a causal voice-pitch tracker on the side chain.
// While the gate is open and the last 25 ms are periodic in a voice's range (YIN at ~12 kHz, 70-400 Hz, evaluated every
// 10 ms), the hold restarts: a soft syllable or a word's voiced tail that dips under the threshold keeps the gate open.
// It never opens the gate (no look-ahead, no latency) and is capped `kMaxExtMs` past the last loud crossing.
class PitchHold
{
public:
    void prepare (double sampleRate) noexcept
    {
        dec = std::max (1, (int) std::lround (sampleRate / 12000.0));
        const double r = sampleRate / dec;
        win = (int) std::lround (0.025 * r);
        tmin = (int) std::floor (r / 400.0);
        tmax = (int) std::ceil (r / 70.0);
        step = std::max (1, (int) std::lround (0.010 * r));
        len = win + tmax;
        ring.assign ((size_t) len, 0.0f);
        lin.assign ((size_t) len, 0.0f);
        d.assign ((size_t) tmax + 1, 0.0);
        // anti-alias: two RBJ low-passes at 0.35 x the decimated rate
        const double w0 = 2.0 * 3.14159265358979323846 * 0.35 * r / sampleRate, q = 0.7071;
        const double al = std::sin (w0) / (2.0 * q), cw = std::cos (w0), a0 = 1.0 + al;
        b0 = (float) ((1.0 - cw) / 2.0 / a0); b1 = (float) ((1.0 - cw) / a0); b2 = b0;
        a1 = (float) (-2.0 * cw / a0); a2 = (float) ((1.0 - al) / a0);
        reset();
    }
    void reset() noexcept
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        for (auto& z : zs) z = 0.0f;
        pos = 0; cnt = 0; dc = 0; periodic = false;
    }
    // one side-chain sample; returns whether the recent signal is a pitched voice at least `minRms` (RMS) strong.
    // `needed`: the answer matters now (the gate is open and under its opening threshold); otherwise the samples are
    // only stored and the analysis is skipped
    bool push (float x, float minRms, bool needed) noexcept
    {
        float y = biquad (x, 0);
        y = biquad (y, 2);
        if (++dc < dec)
            return periodic;
        dc = 0;
        ring[(size_t) pos] = y;
        pos = (pos + 1) % len;
        if (++cnt < step)
            return periodic;
        cnt = 0;
        if (! needed)
            return periodic = false;
        double e = 0.0;
        std::copy (ring.begin() + pos, ring.end(), lin.begin());       // oldest .. newest
        std::copy (ring.begin(), ring.begin() + pos, lin.begin() + (len - pos));
        const float* xw = lin.data() + tmax;                         // the last `win` samples
        for (int j = 0; j < win; ++j)
            e += (double) xw[j] * xw[j];
        if (e < (double) minRms * minRms * win)
            return periodic = false;
        // YIN's difference function, eight lags at a time held in registers (the window is read once per 8 lags)
        int t0 = 1;
        for (; t0 + 8 <= tmax + 1; t0 += 8)
        {
            double acc[8] = {};
            for (int j = 0; j < win; ++j)
            {
                const double xj = (double) xw[j];
                const float* xl = xw + j - t0;
                for (int u = 0; u < 8; ++u)
                {
                    const double v = xj - (double) xl[-u];
                    acc[u] += v * v;
                }
            }
            for (int u = 0; u < 8; ++u)
                d[(size_t) (t0 + u)] = acc[u];
        }
        for (int t = t0; t <= tmax; ++t)
        {
            double acc = 0.0;
            for (int j = 0; j < win; ++j)
            {
                const double v = (double) xw[j] - (double) xw[j - t];
                acc += v * v;
            }
            d[(size_t) t] = acc;
        }
        double cum = 0.0, best = 1.0;
        for (int t = 1; t <= tmax; ++t)
        {
            cum += d[(size_t) t];
            if (t >= tmin && cum > 0.0)
                best = std::min (best, d[(size_t) t] * t / cum);
        }
        return periodic = best < kYinThreshold;
    }
    static constexpr float kMaxExtMs = 500.0f;
    static constexpr double kYinThreshold = 0.25;

private:
    float biquad (float x, int k) noexcept
    {
        const float y = b0 * x + zs[(size_t) k];
        zs[(size_t) k] = b1 * x - a1 * y + zs[(size_t) k + 1];
        zs[(size_t) k + 1] = b2 * x - a2 * y;
        return y;
    }
    std::vector<float> ring, lin;
    std::vector<double> d;
    std::array<float, 4> zs {};
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    int dec = 4, win = 300, tmin = 30, tmax = 172, step = 60, len = 472, pos = 0, cnt = 0, dc = 0;
    bool periodic = false;
};

class Engine
{
public:
    static constexpr int kChunk = 256;
    static constexpr int kMaxEvents = 4096;

    void prepare (double sampleRate, int numChannels, int maxBlockSize = 0)
    {
        fs = sampleRate;
        numCh = std::clamp (numChannels, 1, kMaxCh);
        maxBlock = std::max (0, maxBlockSize);
        va.prepare (fs);
        hop = va.hopSize();
        // a re-prepare at the same rate keeps what the canceller learned
        std::vector<float> keep;
        if (pathCap > 0 && canceller.learnedBlocks() > 0 && std::abs (pathFs - fs) < 1.0e-6)
        {
            keep.resize ((size_t) canceller.pathSize());
            canceller.exportPath (keep.data());
        }
        canceller.prepare (fs);
        if (! keep.empty())
            canceller.importPath (keep.data(), (int) keep.size());
        pathFs = fs;
        pathCap = canceller.pathSize();
        snap.reset (new std::atomic<float>[(size_t) pathCap]);
        pend.reset (new std::atomic<float>[(size_t) pathCap]);
        pathTmp.assign ((size_t) pathCap, 0.0f);
        pendState.store (0);
        snapLen.store (0);
        snapCountdown = 0;
        if (! keep.empty())
            publishPath();
        // the longest latency any configuration needs: BEST vocal (frame delay + 30 hops + pre-open)
        maxLatency = va.frameDelay() + 31 * hop + (int) std::ceil (0.065 * fs) + 8 + canceller.latency();
        int ring = 1;
        while (ring < maxLatency + kChunk + 4)
            ring <<= 1;
        ringMask = ring - 1;
        for (auto& r : delayRing)
            r.assign ((size_t) ring, 0.0f);
        for (auto& r : dryRing)
            r.assign ((size_t) ring, 0.0f);
        for (auto& r : preRing)
            r.assign ((size_t) ring, 0.0f);
        envRing.assign ((size_t) ring, 0.0f);
        keyRing.assign ((size_t) ring, 0.0f);
        bandHp1.setHighPass (fs, 300.0);
        bandHp2.setHighPass (fs, 300.0);
        bandLp1.setLowPass (fs, 6000.0);
        bandLp2.setLowPass (fs, 6000.0);
        breathRise = (float) (3.0 * va.hopSize() / fs); // floor rises 3 dB/s
        double c[12];
        HilbertPair::design (fs, 20.0, 12, c);
        hilbert.setCoefficients (c, 12);
        levelRel = onePole (0.01f, fs);              // peak detector release 10 ms
        pitch.prepare (fs);
        refAtt = 1.0f - onePole (0.005f, fs);        // onset reference: 5 ms attack
        refRelK = 1.0f - onePole (0.05f, fs);        // 50 ms release
        envK = 1.0f - onePole (0.001f, fs);          // analytic envelope smoothing 1 ms
        switchK = 1.0f - onePole (0.01f, fs);
        smoothK = 1.0f - onePole (0.02f, fs);
        fadeStep = (float) (1.0 / std::max (1.0, 0.003 * fs));
        setParams (params);
        active = want;
        applyConfig();
        hs.prepared (latency, fs);
        reset();
    }

    void reset() noexcept
    {
        const bool apply = ! hs.tracking() || latencyOf (want) == latencyOf (active);
        if (apply && want != active)
        {
            active = want;
            applyConfig();
        }
        va.reset();
        hmm.reset();
        canceller.reset();
        for (auto& r : preRing)
            std::fill (r.begin(), r.end(), 0.0f);
        for (auto& r : delayRing)
            std::fill (r.begin(), r.end(), 0.0f);
        for (auto& r : dryRing)
            std::fill (r.begin(), r.end(), 0.0f);
        std::fill (envRing.begin(), envRing.end(), 0.0f);
        std::fill (keyRing.begin(), keyRing.end(), 0.0f);
        scHp1.resetState();
        scHp2.resetState();
        scLp1.resetState();
        scLp2.resetState();
        flushNote = flushNote || noteOn; // a note still sounding gets its note off with the next block
        noteOn = false;
        measureLeft = 0;
        for (auto* f : { &bandHp1, &bandHp2, &bandLp1, &bandLp2 })
            f->resetState();
        breathPow = 0.0f;
        breathCount = 0;
        breathFloor = -140.0f;
        accRaw = accClean = accBeat = accN = gapRaw = gapClean = gapEst = 0.0;
        gapSmp = 0;
        hilbert.resetState();
        writePos = 0;
        inCount = 0;
        evHead = evTail = 0;
        target = closedGain();
        targetDb = gainDbOf (target);
        curDb = targetDb;
        smoothDb = curDb;
        curLin = smoothLin = target;
        levelEnv = 0.0f;
        levelOpen = false;
        levelHold = 0;
        pitch.reset();
        sinceLoud = 1 << 30;
        holdFrames = 0;
        breathSm = 0.0f;
        roomRun = 0;
        roomTailSm = 0.0f;
        lastDecision = 0;
        lastScheduled = target;
        hitActive = false;
        hitPeak = 0.0f;
        hitMinGain = 1.0f;
        refEnv = envSm = 0.0f;
        hitDecaying = false;
        sinceOnset = 1 << 30;
        outGain = outTarget;
        bypassAmt = bypassTarget;
        fadeGain = fadeTgt = (want == active) ? 1.0f : 0.0f;
        warm = 0;
        hopCount = 0;
        hopIn = hopOut = 0.0f;
        hopGain = 0.0f;
        lastFrame = {};
        freePpq = 0.0;
    }

    void setParams (const Params& p) noexcept
    {
        params = p;
        want.mode = std::clamp (p.mode, 0, 2);
        want.speed = want.mode == kVocal ? std::clamp (p.speed, 0, 2) : kLive;
        want.smart = want.mode == kVocal && p.detect == kSmart && want.speed != kLive;
        want.bleed = want.mode == kVocal && p.bleed;
        canceller.setFrozen (p.bleedFreeze);
        outTarget = dbToGain (std::clamp (p.outputDb, -24.0f, 24.0f));
        bypassTarget = p.bypass ? 1.0f : 0.0f;
        rangeLin = dbToGain (std::clamp (p.rangeDb, -80.0f, 0.0f));
        breathLin = dbToGain (std::clamp (p.breathDb, -80.0f, 0.0f));
        const double thrLin = std::pow (10.0, std::clamp (p.thresholdDb, -90.0f, 0.0f) / 20.0);
        thrOpen = (float) thrLin;
        thrClose = (float) (thrLin * std::pow (10.0, -4.0 / 20.0)); // 4 dB hysteresis
        attackSmp = std::max (1.0f, std::clamp (p.attackMs, 0.05f, 50.0f) * 0.001f * (float) fs);
        releaseSmp = std::max (1.0f, std::clamp (p.releaseMs, 1.0f, 2000.0f) * 0.001f * (float) fs);
        holdSmp = (int) (std::clamp (p.holdMs, 0.0f, 2000.0f) * 0.001f * (float) fs);
        pitchOn = p.pitchHold;
        maxExtSmp = (int) (PitchHold::kMaxExtMs * 0.001f * (float) fs);
        tailAlpha = 100.0f / std::clamp (p.tailPct, 5.0f, 100.0f) - 1.0f;
        sensLin = std::pow (10.0f, std::clamp (p.sensDb, 1.0f, 30.0f) / 20.0f);
        openTh = 0.3 - 0.0025 * std::clamp ((double) p.sensitivity, -100.0, 100.0); // 0.55 strict .. 0.05 open
        roomVeto = std::clamp ((double) p.roomVeto, 0.0, 1.0);
        roomRunFrames = std::max (1, (int) std::lround (std::clamp ((double) p.roomRunMs, 2.0, 500.0) * 0.001 * fs / std::max (1, hop)));
        const double hp = std::clamp ((double) p.keyHpHz, 20.0, 2000.0), lp = std::clamp ((double) p.keyLpHz, 1000.0, 20000.0);
        scHp1.setHighPass (fs, hp);
        scHp2.setHighPass (fs, hp);
        keyLpOn = lp < 19999.0 && lp < 0.45 * fs;
        scLp1.setLowPass (fs, std::min (lp, 0.45 * fs));
        scLp2.setLowPass (fs, std::min (lp, 0.45 * fs));
        for (int k = 0; k < 16; ++k)
            stepLvl[(size_t) k] = std::clamp (p.stepLevel[(size_t) k], 0.0f, 1.0f);
    }

    struct Config
    {
        int mode = kVocal, speed = kBest;
        bool smart = true, bleed = false;
        bool operator== (const Config& o) const noexcept { return mode == o.mode && speed == o.speed && smart == o.smart && bleed == o.bleed; }
        bool operator!= (const Config& o) const noexcept { return ! (*this == o); }
    };

    int lagFramesOf (const Config& c) const noexcept
    {
        if (params.lagFrames >= 0)
            return std::min (params.lagFrames, 30);
        return c.speed == kBest ? 5 : 0; // BEST: 10 ms of hidden-Markov look-ahead; FAST: forward only
    }
    int closeEarlyOf (const Config& c) const noexcept
    {
        const double ms = params.closeEarlyMs >= 0.0f ? (double) params.closeEarlyMs : 0.0; // 1.0: = the pre-open
        return std::min (preOpenOf (c), (int) std::lround (std::max (0.0, ms) * 0.001 * fs));
    }

    int preOpenOf (const Config& c) const noexcept
    {
        if (c.mode == kShape)
            return (int) std::lround (0.004 * fs);       // look-ahead: the backtrack (1.5 ms) + the pre-ramp
        if (c.mode == kVocal && c.speed != kLive)
        {
            // open this early before the first voice frame: soft starts (h, f, breathy onsets) are recognised late
            const double ms = params.preOpenMs >= 0.0f ? (double) params.preOpenMs : (c.speed == kBest ? 45.0 : 30.0);
            return (int) std::lround (std::clamp (ms, 0.0, 60.0) * 0.001 * fs);
        }
        return 0;
    }
    int latencyOf (const Config& c) const noexcept { return gateLatencyOf (c) + (c.bleed ? canceller.latency() : 0); }
    int gateLatencyOf (const Config& c) const noexcept
    {
        if (c.mode == kRhythm || (c.mode == kVocal && c.speed == kLive))
            return 0;
        if (c.mode == kShape)
            return preOpenOf (c);
        if (c.smart)
            return va.frameDelay() + lagFramesOf (c) * hop + preOpenOf (c) + hop; // + 1 hop: the frame-quantised decision
        return preOpenOf (c) + (int) std::lround (0.005 * fs);                  // LEVEL with look-ahead
    }
    int getLatencySamples() const noexcept { return latencyPub.load (std::memory_order_acquire); }
    LatencyHandshake& handshake() noexcept { return hs; }
    bool popDisplay (DisplayFrame& f) noexcept { return display.pop (f); }
    double sampleRate() const noexcept { return fs; }
    void setTransport (const Transport& t) noexcept { transport = t; }
    float* gainTap = nullptr; // research / tests: when set, receives the gate gain of every output sample (in order)

    // BLEED persistence (message thread). The audio thread publishes what it has learned twice a second (seqlock);
    // copyBleedPath() reads the latest copy: false if nothing has been learned. setBleedPath() hands a saved path to
    // the audio thread (applied at the start of the next block): false if it does not fit this sample rate.
    bool copyBleedPath (std::vector<float>& out, double& rate) const
    {
        const int n = snapLen.load (std::memory_order_acquire);
        if (n <= 0 || pathCap == 0)
            return false;
        out.resize ((size_t) n);
        for (int attempt = 0; attempt < 1000; ++attempt)
        {
            const uint32_t s1 = snapSeq.load (std::memory_order_acquire);
            if (s1 & 1u)
            {
                std::this_thread::yield();
                continue;
            }
            for (int i = 0; i < n; ++i)
                out[(size_t) i] = snap[(size_t) i].load (std::memory_order_relaxed);
            std::atomic_thread_fence (std::memory_order_acquire);
            if (snapSeq.load (std::memory_order_relaxed) == s1)
            {
                rate = pathFs;
                return true;
            }
        }
        return false;
    }

    bool setBleedPath (const float* data, int n, double rate)
    {
        if (pathCap == 0 || n != pathCap || std::abs (rate - pathFs) > 1.0e-6)
            return false;
        for (int spin = 0;; ++spin)
        {
            int st = pendState.load (std::memory_order_acquire);
            if ((st == 0 || st == 2) && pendState.compare_exchange_weak (st, 1, std::memory_order_acq_rel))
                break;
            if (spin > 1000000)
                return false;
        }
        for (int i = 0; i < n; ++i)
            pend[(size_t) i].store (data[i], std::memory_order_relaxed);
        pendState.store (2, std::memory_order_release);
        return true;
    }

    // SHAPE: the MIDI notes of the last process() call (one per hit, 50 ms long; offsets within that block)
    int midiEvents (const MidiEvent*& events) const noexcept
    {
        events = midiBuf.data();
        return midiCount;
    }

    // the BLEED meter: how much quieter the mic is where the bleed is plain (dB, smoothed over ~2 s), the fit of the estimate (0..1:
    // the safety weight) and the beat's level on the side-chain (dBFS)
    float bleedGapDb() const noexcept { return meterGapDb.load (std::memory_order_relaxed); }
    float bleedFit() const noexcept { return meterFit.load (std::memory_order_relaxed); }
    float bleedBeatDb() const noexcept { return meterBeatDb.load (std::memory_order_relaxed); }
    bool bleedLearned() const noexcept { return snapLen.load (std::memory_order_relaxed) > 0; }

    // sc: the side-chain (the beat for BLEED), scCh 0 when there is none
    void process (float* const* io, int numChannels, int numSamples, const float* const* sc = nullptr, int scCh = 0) noexcept
    {
        ScopedFtz ftz;
        const int nCh = std::clamp (numChannels, 0, numCh);
        if (nCh == 0)
            return;
        double ppq = transport.ppq;
        midiCount = 0;
        if (flushNote)
        {
            addMidi (0, curNote, 0);
            flushNote = false;
        }
        for (int off = 0; off < numSamples; off += kChunk)
        {
            const int n = std::min (kChunk, numSamples - off);
            float* p[kMaxCh] {};
            const float* q[2] {};
            for (int c = 0; c < nCh; ++c)
                p[c] = io[c] + off;
            for (int c = 0; c < std::min (scCh, 2); ++c)
                q[c] = sc[c] + off;
            chunkOffset = off;
            chunk (p, nCh, n, ppq, q, std::min (scCh, 2));
            ppq += (double) n * transport.bpm / (60.0 * fs);
        }
    }

private:
    float closedGain() const noexcept { return rangeLin; }
    static float gainDbOf (float g) noexcept { return 20.0f * std::log10 (std::max (g, 1.0e-6f)); }

    void applyConfig() noexcept
    {
        hmm.setup (lagFramesOf (active), 0.998, 0.995, (double) vmodel::kPriorVoice, params.resumePerFrame, params.toLongPerFrame);
        hmm.reset();
        latency = latencyOf (active);
        latencyPub.store (latency, std::memory_order_release);
        evHead = evTail = 0;
        measureLeft = 0; // the event whose velocity was being measured is gone
    }

    void control (int n) noexcept
    {
        const bool ready = hs.ready (latencyOf (hs.isHolding() ? active : want), fadeGain <= 0.0f, n);
        const Config tgt = hs.isHolding() ? active : want;
        if (tgt != active)
        {
            fadeTgt = 0.0f;
            if (fadeGain <= 0.0f && ready)
            {
                active = tgt;
                applyConfig();
                warm = latency + (hs.tracking() ? maxBlock : 0);
            }
        }
        else
            fadeTgt = (! ready || warm > 0) ? 0.0f : 1.0f;
        if (warm > 0)
            warm = std::max (0, warm - n);
    }

    // an event: from input-timeline sample `at` on, the gate's target gain is `g`
    bool schedule (int64_t at, float g, uint8_t kind = 0) noexcept
    {
        const int next = (evTail + 1) % kMaxEvents;
        if (next == evHead)
            return false;
        evAt[(size_t) evTail] = at;
        evGain[(size_t) evTail] = g;
        evKind[(size_t) evTail] = kind;
        evTail = next;
        return true;
    }

    // VOCAL SMART: one 2 ms frame of voice activity -> a decision for the frame `lag` frames back
    void voiceFrame (const VoiceFrame& f) noexcept
    {
        lastFrame = f;
        // 2.0, the room veto: once the room model has explained the signal without a break for roomRunMs (every
        // frame fully predicted by the room's decay and falling at its rate), the voice has stopped - what rings on is
        // the room. Voice evidence (the network's and the level's) is withdrawn from then on, until the room no
        // longer explains a frame. Single room-like frames inside words (between syllables) are not affected
        roomTailSm += 0.2f * (f.roomTail - roomTailSm); // ~10 ms: one noisy frame does not break a tail
        roomRun = roomTailSm > 0.5f ? roomRun + 1 : 0;
        const double veto = roomRun >= roomRunFrames ? roomVeto : 0.0;
        const double pv = std::max ((double) f.pVoice, (double) f.loud) * (1.0 - veto);
        double post = 0.0;
        // breath evidence, smoothed over a few frames. A breath is an event above the room: a steady noise floor
        // (aperiodic and broadband, like a breath) never rises above itself - require the breath band (300 Hz - 6 kHz,
        // where a breath has its energy; rumble cannot fake it) to be 6 dB above its own floor
        const float bandDb = 10.0f * std::log10 (breathPow / (float) std::max (1, breathCount) + 1.0e-20f);
        breathPow = 0.0f;
        breathCount = 0;
        if (bandDb > -140.0f) // digital silence is not a floor (2.0: it held the floor at -200 dB for a minute)
            breathFloor = (bandDb < breathFloor || breathFloor < -139.0f) ? bandDb : breathFloor + breathRise; // slowly rising minimum
        const float aboveFloor = 1.0f / (1.0f + std::exp (-(bandDb - breathFloor - 6.0f) / 1.0f));
        breathSm += 0.3f * (f.pBreath * aboveFloor - breathSm);
        breathRing[(size_t) (frameIdx % FixedLagHmm::kMaxLag)] = breathSm;
        centerRing[(size_t) (frameIdx % FixedLagHmm::kMaxLag)] = f.center;
        const int lag = hmm.lagFrames();
        ++frameIdx;
        if (! hmm.push (pv, post))
            return;
        const int64_t k = frameIdx - 1 - lag;
        const int64_t center = centerRing[(size_t) (k % FixedLagHmm::kMaxLag)];
        const float br = breathRing[(size_t) (k % FixedLagHmm::kMaxLag)];
        int decision = post > openTh ? 2 : (br > 0.5f ? 1 : 0); // 2 voice, 1 breath, 0 background
        if (decision == 2)
            holdFrames = holdSmp / std::max (1, hop);
        else if (holdFrames > 0)
        {
            --holdFrames;
            decision = 2;
        }
        if (decision != lastDecision)
        {
            lastDecision = decision;
            const float g = decision == 2 ? 1.0f : decision == 1 ? std::max (breathLin, rangeLin) : rangeLin;
            const bool up = g > lastScheduled;
            lastScheduled = g;
            // opening runs early (soft starts are recognised late); closing lands at its own time after the hold. An
            // event that overtakes pending ones replaces them: a pause shorter than the pre-open never closes
            scheduleReplacing (center - (up ? preOpenOf (active) : closeEarlyOf (active)) - hop / 2, g, (uint8_t) decision);
        }
    }

    void scheduleReplacing (int64_t at, float g, uint8_t kind) noexcept
    {
        while (evTail != evHead && evAt[(size_t) ((evTail - 1 + kMaxEvents) % kMaxEvents)] >= at)
            evTail = (evTail - 1 + kMaxEvents) % kMaxEvents;
        schedule (at, g, kind);
    }

    // VOCAL LEVEL: classic peak detector on the 80 Hz high-passed side chain, hysteresis, hold
    void levelSample (float x, int64_t at) noexcept
    {
        float s = (float) scHp2.process (scHp1.process (x, 0), 0);
        if (keyLpOn)
            s = (float) scLp2.process (scLp1.process (s, 0), 0);
        keyRing[(size_t) (at & ringMask)] = s;
        const float a = std::abs (s);
        levelEnv = a > levelEnv ? a : levelEnv * levelRel;
        bool open = levelOpen;
        if (pitchOn)
        {
            // 4.0 PITCH HOLD: a pitched voice (RMS no lower than 26 dB under the closing threshold) keeps an open gate open
            const bool pitched = pitch.push (s, 0.05f * thrClose, levelOpen && levelEnv <= thrOpen);
            sinceLoud = levelEnv > thrOpen ? 0 : std::min (sinceLoud + 1, 1 << 30);
            if (levelOpen && pitched && sinceLoud < maxExtSmp)
                levelHold = holdSmp;
        }
        if (levelEnv > thrOpen)
        {
            open = true;
            levelHold = holdSmp;
        }
        else if (levelEnv < thrClose)
        {
            if (levelHold > 0)
                --levelHold;
            else
                open = false;
        }
        if (open != levelOpen)
        {
            levelOpen = open;
            schedule (at - (open ? preOpenOf (active) : 0), open ? 1.0f : rangeLin, open ? 2 : 0);
        }
    }

    // SHAPE: onsets on the input; the per-hit decay scaling is applied at the output from the stored envelope
    void shapeSample (float x, int64_t at) noexcept
    {
        const float a = std::abs (x);
        // onset: the instantaneous level jumps `sens` dB above a peak follower of the recent level (5 ms attack,
        // 50 ms release): a hit's own rise and decay never re-trigger it, the next hit does
        const bool onset = a > thrOpen && a > sensLin * (refEnv + 1.0e-9f) && sinceOnset > (int) (0.02 * fs);
        refEnv += (a - refEnv) * (a > refEnv ? refAtt : refRelK);
        ++sinceOnset;
        if (onset)
        {
            sinceOnset = 0;
            // the opening ramp ends at the onset; decay scaling starts after the hit's peak
            // a hit over the previous one's ringing tail is only recognised once it has risen `sens` dB above it -
            // its first rise starts earlier: the opening ramp ends 1.5 ms before the detection (look-ahead)
            const float back = (float) (0.0015 * fs);
            schedule (at - (int64_t) std::min (attackSmp + back, (float) preOpenOf (active)), 1.0f, 2);
            // 2 = "a hit starts here": the decay is tracked from the detection on. 2.0: the event also carries the
            // hit's MIDI velocity (2 + velocity), measured on the next 3 ms of input before the output reaches it
            measureSlot = evTail;
            if (schedule (at, 2.0f, 3))
            {
                measurePeak = a;
                measureLeft = (int) (0.003 * fs);
            }
        }
        else if (measureLeft > 0)
        {
            measurePeak = std::max (measurePeak, a);
            if (--measureLeft == 0)
            {
                // velocity: -48 dBFS -> 1 .. 0 dBFS -> 127, linear in dB
                const float dbv = 20.0f * std::log10 (std::max (measurePeak, 1.0e-9f));
                const float vel = std::round (1.0f + 126.0f * std::clamp ((dbv + 48.0f) / 48.0f, 0.0f, 1.0f));
                evGain[(size_t) measureSlot] = 2.0f + vel;
            }
        }
    }

    float rhythmGain (double ppq) const noexcept
    {
        static constexpr double kStepQ[6] = { 1.0, 0.5, 0.25, 0.125, 1.0 / 3.0, 1.0 / 6.0 };
        const double stepQ = kStepQ[std::clamp (params.rate, 0, 5)];
        double pos = ppq / stepQ;                      // steps
        // swing: every second step starts later by swing% of half a step
        const double sw = std::clamp ((double) params.swingPct, 0.0, 100.0) * 0.005;
        const double pair = std::floor (pos / 2.0) * 2.0, inPair = pos - pair;
        const double warp = inPair < 1.0 + sw ? inPair / (1.0 + sw) : 1.0 + (inPair - 1.0 - sw) / (1.0 - sw);
        pos = pair + warp;
        const double stepSec = stepQ * 60.0 / std::max (1.0, transport.bpm);
        const double edge = std::clamp ((double) params.edgeMs * 0.001 / stepSec, 1.0e-4, 0.5); // in steps
        const int64_t step = (int64_t) std::floor (pos);
        const double f = pos - (double) step;
        const int len = std::clamp (params.steps, 2, 16);
        auto on = [this, len] (int64_t s) {
            const int k = (int) (((s % len) + len) % len);
            return ((params.pattern >> k) & 1u) ? (double) stepLvl[(size_t) k] : 0.0;
        };
        double g = on (step);
        // raised-cosine crossfades centred on the step boundaries
        if (f < edge * 0.5)
        {
            const double u = 0.5 + f / edge;
            const double w = 0.5 - 0.5 * std::cos (kPi * u);
            g = on (step - 1) + (on (step) - on (step - 1)) * w;
        }
        else if (f > 1.0 - edge * 0.5)
        {
            const double u = (f - (1.0 - edge * 0.5)) / edge;
            const double w = 0.5 - 0.5 * std::cos (kPi * u);
            g = on (step) + (on (step + 1) - on (step)) * w;
        }
        return (float) (rangeLin + (1.0 - rangeLin) * g);
    }

    void addMidi (int offset, uint8_t note, uint8_t velocity) noexcept
    {
        if (midiCount < (int) midiBuf.size())
            midiBuf[(size_t) midiCount++] = { offset, note, velocity };
    }

    void publishPath() noexcept
    {
        canceller.exportPath (pathTmp.data());
        const uint32_t s0 = snapSeq.load (std::memory_order_relaxed);
        snapSeq.store (s0 + 1, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);
        for (int i = 0; i < pathCap; ++i)
            snap[(size_t) i].store (pathTmp[(size_t) i], std::memory_order_relaxed);
        snapSeq.store (s0 + 2, std::memory_order_release);
        snapLen.store (pathCap, std::memory_order_release);
    }

    void bleedHousekeeping (int n) noexcept
    {
        int st = 2;
        if (pendState.compare_exchange_strong (st, 3, std::memory_order_acq_rel))
        {
            for (int i = 0; i < pathCap; ++i)
                pathTmp[(size_t) i] = pend[(size_t) i].load (std::memory_order_relaxed);
            pendState.store (0, std::memory_order_release);
            if (canceller.importPath (pathTmp.data(), pathCap))
                publishPath();
        }
        if (active.bleed)
        {
            snapCountdown -= n;
            if (snapCountdown <= 0)
            {
                snapCountdown = (int) (0.5 * fs);
                if (canceller.learnedBlocks() > 0)
                    publishPath();
            }
            const double k = std::exp (-(double) n / (2.0 * fs));
            accRaw *= k;
            accClean *= k;
            accBeat *= k;
            accN *= k;
            meterGapDb.store (accRaw > 1.0e-14 ? (float) (10.0 * std::log10 ((accRaw + 1.0e-20) / (accClean + 1.0e-20))) : 0.0f, std::memory_order_relaxed);
            meterFit.store (canceller.estimateWeight(), std::memory_order_relaxed);
            meterBeatDb.store (accN > 0.0 ? (float) (10.0 * std::log10 (accBeat / accN + 1.0e-20)) : -120.0f, std::memory_order_relaxed);
        }
    }

    void chunk (float* const* io, int nCh, int n, double ppq, const float* const* sc, int scCh) noexcept
    {
        control (n);
        if (pathCap > 0)
            bleedHousekeeping (n);
        const int L = latency - (active.bleed ? canceller.latency() : 0); // the gate's own delay
        const int Ltot = latency;
        const int Dc = canceller.latency();
        const int mode = active.mode;
        // 1. input: sanitize, rings, BLEED, detection (input timeline of the cleaned signal)
        for (int i = 0; i < n; ++i)
        {
            float raw = 0.0f;
            for (int c = 0; c < nCh; ++c)
            {
                float v = io[c][i];
                v = std::isfinite (v) ? std::clamp (v, -1000.0f, 1000.0f) : 0.0f;
                io[c][i] = v;
                dryRing[(size_t) c][(size_t) ((writePos + i) & ringMask)] = v;
                preRing[(size_t) c][(size_t) ((writePos + i) & ringMask)] = v;
                raw += v;
                hopIn = std::max (hopIn, std::abs (v));
            }
            raw /= (float) nCh;
            float est = 0.0f;
            if (active.bleed)
            {
                auto clean = [] (float v) { return std::isfinite (v) ? std::clamp (v, -1000.0f, 1000.0f) : 0.0f; };
                const float rL = scCh > 0 ? clean (sc[0][i]) : 0.0f, rR = scCh > 1 ? clean (sc[1][i]) : rL;
                canceller.process (raw, rL, rR, est); // the bleed in the mic sample Dc samples ago
                accBeat += 0.5 * ((double) rL * rL + (double) rR * rR);
                accN += 1.0;
                // where the bleed is plain: a 2 ms stretch counts when the (fitted) estimate carries 90 % of the mic's
                // energy there - the gaps, whatever the gate decides; the voice and a misfit estimate do not count. A
                // conservative reading (measured: 22 dB shown for 30 dB removed, 7 dB for 8-11 dB on a real rap take)
                const float rm = preRing[0][(size_t) ((writePos + i - Dc) & ringMask)];
                gapRaw += (double) rm * rm;
                gapClean += (double) (rm - est) * (rm - est);
                gapEst += (double) est * est;
                if (++gapSmp >= hop)
                {
                    if (gapEst >= 0.9 * gapRaw && gapRaw > 1.0e-14)
                    {
                        accRaw += gapRaw;
                        accClean += gapClean;
                    }
                    gapRaw = gapClean = gapEst = 0.0;
                    gapSmp = 0;
                }
            }
            // the detectors listen to the mic as recorded (what they were trained and measured on; aligned with the
            // cleaned signal); the gate acts on the cleaned signal
            float m = 0.0f;
            for (int c = 0; c < nCh; ++c)
            {
                // with BLEED: every channel Dc samples late, minus the estimated bleed of that sample
                const float r = active.bleed ? preRing[(size_t) c][(size_t) ((writePos + i - Dc) & ringMask)] : io[c][i];
                delayRing[(size_t) c][(size_t) ((writePos + i) & ringMask)] = active.bleed ? r - est : r;
                m += r;
            }
            m /= (float) nCh;
            const int64_t at = inCount + i;
            if (mode == kVocal)
            {
                if (active.smart)
                {
                    const double bb = bandLp2.process (bandLp1.process (bandHp2.process (bandHp1.process (m, 0), 0), 0), 0);
                    breathPow += (float) (bb * bb);
                    ++breathCount;
                    if (va.push (m))
                        voiceFrame (va.out());
                }
                else
                    levelSample (m, at);
            }
            else if (mode == kShape)
            {
                envSm += ((float) std::sqrt (hilbert.power (m, 0)) - envSm) * envK;
                envRing[(size_t) ((writePos + i) & ringMask)] = envSm;
                shapeSample (m, at);
            }
        }
        // 2. output: the delayed signal times the gain
        const float upRate = (0.0f - gainDbOf (rangeLin)) / attackSmp, downRate = (0.0f - gainDbOf (rangeLin)) / releaseSmp;
        const float cornerK = 1.0f - std::exp (-1.0f / std::max (2.0f, 0.15f * std::min (attackSmp, releaseSmp)));
        double rppq = ppq;
        const double ppqStep = transport.bpm / (60.0 * fs);
        for (int i = 0; i < n; ++i)
        {
            const int64_t s = inCount + i - L; // output timeline (input-sample index)
            while (evHead != evTail && evAt[(size_t) evHead] <= s)
            {
                const float g = evGain[(size_t) evHead];
                outKind = evKind[(size_t) evHead] == 3 ? (uint8_t) 2 : evKind[(size_t) evHead];
                hitInHop = hitInHop || evKind[(size_t) evHead] == 3;
                if (g > 1.5f)
                {
                    if (mode == kShape) // MIDI: the previous note ends, this hit's note starts
                    {
                        if (noteOn)
                            addMidi (chunkOffset + i, curNote, 0);
                        curNote = (uint8_t) std::clamp (params.midiNote, 0, 127);
                        addMidi (chunkOffset + i, curNote, (uint8_t) std::clamp ((int) std::lround (g - 2.0f), 1, 127));
                        noteOn = true;
                        noteOffAt = s + (int64_t) (0.05 * fs);
                    }
                    hitActive = true; // SHAPE: a new hit from this sample on
                    hitDecaying = false;
                    hitPeak = 0.0f;
                    hitMinGain = 1.0f;
                    hitStart = s;
                    target = 1.0f;
                    shapeGateUntil = s + holdSmp;
                }
                else
                    target = g;
                targetDb = gainDbOf (target);
                evHead = (evHead + 1) % kMaxEvents;
            }
            if (noteOn && (s >= noteOffAt || mode != kShape))
            {
                addMidi (chunkOffset + i, curNote, 0);
                noteOn = false;
            }
            float g;
            if (mode == kRhythm)
            {
                const double p = transport.valid && transport.playing ? rppq : freePpq; // stopped: free-running
                g = rhythmGain (p);
                rppq += ppqStep;
                freePpq += ppqStep;
            }
            else
            {
                if (mode == kShape && hitActive)
                {
                    // decay scaling: the hit's own envelope against its running peak, to the power alpha, monotone
                    const float e = envRing[(size_t) ((writePos + i - L) & ringMask)];
                    hitPeak = std::max (hitPeak, e);
                    // the decay starts once the level has fallen 0.25 dB below the hit's peak, at least 5 ms in (a noisy
                    // hit's envelope flickers during its attack; that is not its decay)
                    if (! hitDecaying && s > hitStart + (int64_t) (0.005 * fs) && e < 0.97f * hitPeak)
                        hitDecaying = true;
                    if (hitDecaying)
                    {
                        const float ds = std::pow (std::max (e, 1.0e-9f) / hitPeak, tailAlpha);
                        hitMinGain = std::min (hitMinGain, std::min (1.0f, ds)); // monotone: a tail never swells
                    }
                    // the gate closes when the hit has decayed below the threshold (after the hold)
                    if (hitDecaying && s > shapeGateUntil && e < thrClose)
                    {
                        hitActive = false;
                        target = rangeLin;
                        targetDb = gainDbOf (target);
                    }
                    else
                    {
                        target = std::max (rangeLin, hitMinGain);
                        targetDb = gainDbOf (target);
                    }
                }
                if (mode == kShape && hitActive && hitDecaying)
                {
                    // decay scaling: the gain follows the hit's own (smooth) envelope directly - the RELEASE time only
                    // shapes the final close after the hit
                    curLin = std::min (curLin, target);
                    smoothLin += (curLin - smoothLin) * cornerK;
                    g = smoothLin;
                    curDb = gainDbOf (curLin);
                }
                else if (params.rampLaw == 1)
                {
                    // ramps linear in amplitude (full range in attack / release time), corners rounded
                    const float span = 1.0f - rangeLin;
                    if (target > curLin)
                        curLin = std::min (target, curLin + span / attackSmp);
                    else
                        curLin = std::max (target, curLin - span / releaseSmp);
                    smoothLin += (curLin - smoothLin) * cornerK;
                    g = smoothLin;
                }
                else
                {
                    // ramps in dB (full range in attack / release time), corners rounded by a short one-pole
                    if (targetDb > curDb)
                        curDb = std::min (targetDb, curDb + upRate);
                    else
                        curDb = std::max (targetDb, curDb - downRate);
                    smoothDb += (curDb - smoothDb) * cornerK;
                    g = dbToGain (smoothDb);
                }
            }
            hopGain = g;
            if (gainTap != nullptr)
                *gainTap++ = g;
            const int at = (writePos + i - L) & ringMask;
            glide (outGain, outTarget, smoothK);
            glide (bypassAmt, bypassTarget, switchK);
            fadeGain = fadeTgt > fadeGain ? std::min (fadeTgt, fadeGain + fadeStep) : std::max (fadeTgt, fadeGain - fadeStep);
            const bool listen = mode == kVocal && ! active.smart && params.keyListen;
            for (int c = 0; c < nCh; ++c)
            {
                const float x = delayRing[(size_t) c][(size_t) at];
                float y = listen ? keyRing[(size_t) at] * outGain : x * g * outGain;
                const float dry = dryRing[(size_t) c][(size_t) ((writePos + i - Ltot) & ringMask)];
                if (bypassAmt >= 1.0f)
                    y = dry;
                else if (bypassAmt > 0.0f)
                    y += (dry - y) * bypassAmt;
                y *= fadeGain;
                io[c][i] = y;
                hopOut = std::max (hopOut, std::abs (y));
            }
            if (++hopCount >= hop)
                pushDisplay();
        }
        inCount += n;
        writePos = (writePos + n) & ringMask;
    }

    static void glide (float& v, float tgt, float k) noexcept
    {
        v += (tgt - v) * k;
        if (std::abs (tgt - v) <= 1.0e-4f * std::max (1.0f, std::abs (tgt)))
            v = tgt;
    }

    void pushDisplay() noexcept
    {
        auto db = [] (float a) { return a > 1.0e-6f ? 20.0f * std::log10 (a) : -120.0f; };
        DisplayFrame f;
        f.inDb = db (hopIn);
        f.outDb = db (hopOut);
        f.gainDb = db (hopGain);
        f.pVoice = lastFrame.pVoice;
        f.pBreath = lastFrame.pBreath;
        f.floorDb = lastFrame.floorDb;
        f.state = outKind;
        f.hit = hitInHop;
        hitInHop = false;
        display.push (f);
        hopCount = 0;
        hopIn = hopOut = 0.0f;
    }

    double fs = 48000.0;
    int numCh = 2, maxBlock = 0, hop = 96, maxLatency = 0, latency = 0, warm = 0, ringMask = 0, writePos = 0, hopCount = 0;
    int64_t inCount = 0, frameIdx = 0;
    Params params;
    Config active, want;
    Transport transport;
    VoiceActivity va;
    FixedLagHmm hmm;
    // BLEED persistence and meter
    std::unique_ptr<std::atomic<float>[]> snap, pend;
    std::vector<float> pathTmp;
    std::atomic<uint32_t> snapSeq { 0 };
    std::atomic<int> snapLen { 0 }, pendState { 0 }; // pend: 0 free, 1 being written, 2 ready, 3 being read
    int pathCap = 0, snapCountdown = 0;
    double pathFs = 0.0, accRaw = 0.0, accClean = 0.0, accBeat = 0.0, accN = 0.0;
    std::atomic<float> meterGapDb { 0.0f }, meterFit { 0.0f }, meterBeatDb { -120.0f };
    int gapSmp = 0;
    double gapRaw = 0.0, gapClean = 0.0, gapEst = 0.0;
    LatencyHandshake hs;
    HistoryFifo<DisplayFrame, 4096> display;
    std::atomic<int> latencyPub { 0 };
    std::array<std::vector<float>, kMaxCh> delayRing, dryRing, preRing;
    Canceller canceller;
    std::vector<float> envRing;
    std::array<int64_t, kMaxEvents> evAt {};
    std::array<uint8_t, kMaxEvents> evKind {};
    uint8_t outKind = 0;
    bool hitInHop = false;
    std::array<float, kMaxEvents> evGain {};
    int evHead = 0, evTail = 0;
    std::array<float, FixedLagHmm::kMaxLag> breathRing {};
    std::array<int64_t, FixedLagHmm::kMaxLag> centerRing {};
    Biquad scHp1, scHp2, scLp1, scLp2, bandHp1, bandHp2, bandLp1, bandLp2;
    bool keyLpOn = false;
    std::vector<float> keyRing;
    std::array<float, 16> stepLvl {};
    // SHAPE MIDI
    std::array<MidiEvent, 256> midiBuf {};
    int midiCount = 0, chunkOffset = 0, measureSlot = 0, measureLeft = 0;
    float measurePeak = 0.0f;
    bool noteOn = false, flushNote = false;
    uint8_t curNote = 36;
    int64_t noteOffAt = 0;
    float breathPow = 0.0f, breathFloor = 0.0f, breathRise = 0.006f;
    int breathCount = 0;
    HilbertPair hilbert;
    VoiceFrame lastFrame;
    float target = 0.001f, targetDb = -60.0f, curDb = -60.0f, smoothDb = -60.0f, curLin = 0.001f, smoothLin = 0.001f;
    float rangeLin = 0.001f, breathLin = 0.25f, thrOpen = 0.005f, thrClose = 0.003f;
    float attackSmp = 96.0f, releaseSmp = 3840.0f, tailAlpha = 0.0f, sensLin = 2.8f;
    float lastScheduled = 0.0f;
    double roomVeto = 0.0;
    int roomRun = 0, roomRunFrames = 15;
    float roomTailSm = 0.0f;
    int holdSmp = 2880, holdFrames = 0, levelHold = 0, lastDecision = 0, sinceOnset = 1 << 30;
    PitchHold pitch;
    bool pitchOn = false;
    int sinceLoud = 1 << 30, maxExtSmp = 24000;
    double openTh = 0.3, freePpq = 0.0;
    float levelEnv = 0.0f, levelRel = 0.9995f, breathSm = 0.0f;
    bool levelOpen = false, hitActive = false;
    float hitPeak = 0.0f, hitMinGain = 1.0f, refEnv = 0.0f, refAtt = 0.004f, refRelK = 0.0004f, envK = 0.04f, envSm = 0.0f;
    bool hitDecaying = false;
    int64_t shapeGateUntil = 0, hitStart = 0;
    float outGain = 1.0f, outTarget = 1.0f, bypassAmt = 0.0f, bypassTarget = 0.0f;
    float switchK = 0.002f, smoothK = 0.001f, fadeGain = 1.0f, fadeTgt = 1.0f, fadeStep = 0.01f;
    float hopIn = 0.0f, hopOut = 0.0f, hopGain = 0.0f;
};
} // namespace axg
