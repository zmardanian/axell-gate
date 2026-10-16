// Axell Gate - DSP suite (JUCE-free: the Source/ headers exactly as they ship). Exit code = failed checks.
//   GateDspTests          44.1 / 48 / 96 kHz + CPU benchmark
//   GateDspTests --quick  48 kHz only, no CPU benchmark
#include "Engine.h"
#include "Presets.h"

#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace axg;
static int fails = 0, checks = 0;
static thread_local bool countAllocs = false;
static long allocCount = 0;
void* operator new (std::size_t n)
{
    if (countAllocs)
        ++allocCount;
    if (void* p = std::malloc (n == 0 ? 1 : n))
        return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t n) { return operator new (n); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

static void check (const std::string& name, double v, double lo, double hi, const char* unit)
{
    ++checks;
    const bool ok = v >= lo && v <= hi;
    fails += ok ? 0 : 1;
    std::printf ("%s  %-100s %11.4f %s  [%g .. %g]\n", ok ? "PASS" : "FAIL", name.c_str(), v, unit, lo, hi);
}
static double db (double a) { return 20.0 * std::log10 (std::max (a, 1e-30)); }
static std::string rateName (double fs) { char b[32]; std::snprintf (b, sizeof b, "%.1f kHz", fs / 1000.0); return b; }

using Buf = std::vector<std::vector<float>>;
struct Out
{
    Buf y;
    std::vector<float> g; // gate gain per output sample (aligned with the input)
    int latency = 0;
};
// render with the latency removed; sc = side-chain (may be empty); bpm > 0 runs a host transport from ppq 0
static Out render (double fs, const Params& p, Buf x, int block = 512, const Buf& sc = {}, double bpm = 0.0, bool count = false)
{
    Engine e;
    e.setParams (p);
    e.prepare (fs, (int) x.size(), std::max (block, 512));
    Out o;
    o.latency = e.getLatencySamples();
    const size_t n = x[0].size(), total = n + (size_t) o.latency;
    for (auto& c : x)
        c.resize (total, 0.0f);
    Buf s = sc;
    for (auto& c : s)
        c.resize (total, 0.0f);
    std::vector<float> gains (total, 0.0f);
    e.gainTap = gains.data();
    for (size_t pos = 0; pos < total; pos += (size_t) block)
    {
        const int len = (int) std::min<size_t> ((size_t) block, total - pos);
        float* io[2] {};
        const float* q[2] {};
        for (size_t c = 0; c < x.size(); ++c)
            io[c] = x[c].data() + pos;
        for (size_t c = 0; c < s.size() && c < 2; ++c)
            q[c] = s[c].data() + pos;
        Transport t;
        if (bpm > 0.0)
        {
            t.valid = t.playing = true;
            t.bpm = bpm;
            t.ppq = (double) pos / fs * bpm / 60.0;
        }
        e.setTransport (t);
        countAllocs = count;
        e.process (io, (int) x.size(), len, q, (int) s.size());
        countAllocs = false;
    }
    for (auto& c : x)
        c.erase (c.begin(), c.begin() + o.latency);
    o.y = x;
    o.g.assign (gains.begin() + o.latency, gains.end());
    return o;
}

// synthetic voice: vowels (pulse train through formants, f0 120-170 Hz) with sibilants, phrases separated by pauses,
// over a pink-ish noise floor; voice[i] = true inside phrases
struct Voice
{
    Buf x;
    std::vector<uint8_t> active;
};
static Voice makeVoice (double fs, double secs, double gainDb = -18.0, double noiseDb = -66.0, unsigned seed = 5)
{
    Voice v;
    const size_t n = (size_t) (secs * fs);
    v.x.assign (2, std::vector<float> (n, 0.0f));
    v.active.assign (n, 0);
    std::mt19937 g (seed);
    std::normal_distribution<double> nd;
    const double gain = std::pow (10.0, gainDb / 20.0), ng = std::pow (10.0, noiseDb / 20.0);
    double ph = 0.0, b0 = 0, b1 = 0, s1 = 0, s2 = 0;
    const double w = 2 * kPi * 7000 / fs, r = std::exp (-kPi * 1500 / fs);
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / fs;
        const double phrase = std::fmod (t, 2.4);           // 1.6 s of words, 0.8 s pause
        const bool on = phrase > 0.2 && phrase < 1.8;
        const double syl = std::fmod (phrase, 0.32);        // syllables: 0.22 s vowel, then 0.1 s consonant
        double y = 0.0;
        if (on)
        {
            const double f0 = 140.0 + 25.0 * std::sin (2 * kPi * 0.6 * t);
            ph += 2 * kPi * f0 / fs;
            if (syl < 0.22)
            {
                const double env = std::min (1.0, std::min (syl, 0.22 - syl) / 0.015);
                double s = 0;
                for (int h = 1; h * f0 < std::min (0.45 * fs, 8000.0); ++h)
                {
                    const double f = h * f0;
                    s += (std::exp (-std::pow ((f - 650) / 250, 2)) + 0.6 * std::exp (-std::pow ((f - 1150) / 300, 2))
                          + 0.2 * std::exp (-std::pow ((f - 2600) / 500, 2))) * std::sin (h * ph) / std::sqrt ((double) h);
                }
                y = 0.35 * env * s;
            }
            else
            {
                const double a = (1 - r) * nd (g) + 2 * r * std::cos (w) * s1 - r * r * s2;
                s2 = s1;
                s1 = a;
                y = 0.8 * a * std::sin (kPi * (syl - 0.22) / 0.1);
            }
            v.active[i] = 1;
        }
        // pink-ish noise floor (two one-pole bands)
        const double wn = nd (g);
        b0 = 0.997 * b0 + 0.03 * wn;
        b1 = 0.6 * b1 + 0.4 * wn;
        const double nz = ng * (b0 + 0.3 * b1);
        v.x[0][i] = v.x[1][i] = (float) (gain * y + nz);
    }
    return v;
}

static Buf sine (double fs, double f, double ampDb, double secs, int nc = 1)
{
    Buf b ((size_t) nc, std::vector<float> ((size_t) (secs * fs)));
    const double a = std::pow (10.0, ampDb / 20.0);
    for (size_t i = 0; i < b[0].size(); ++i)
        for (auto& c : b)
            c[i] = (float) (a * std::sin (2 * kPi * f * (double) i / fs));
    return b;
}

// drum hits (one every `every` s): kick-like (pitched sweep), 808-like (long sine), snare-like (noise + tone)
static Buf hits (double fs, int kind, double secs, double every = 1.0)
{
    Buf b (1, std::vector<float> ((size_t) (secs * fs), 0.0f));
    std::mt19937 g (9);
    std::normal_distribution<double> nd;
    double s1 = 0, s2 = 0;
    const double w = 2 * kPi * 4000 / fs, r = std::exp (-kPi * 3000 / fs);
    for (double t0 = 0.1; t0 + every <= secs; t0 += every)
    {
        double ph = 0;
        const size_t a = (size_t) (t0 * fs), len = (size_t) ((every - 0.05) * fs);
        for (size_t i = 0; i < len && a + i < b[0].size(); ++i)
        {
            const double t = (double) i / fs;
            double y = 0;
            if (kind == 0)
            {
                ph += 2 * kPi * (45 + 110 * std::exp (-t * 35)) / fs;
                y = 0.7 * std::sin (ph) * std::exp (-t * 7);
            }
            else if (kind == 1)
            {
                ph += 2 * kPi * 49 * (1 + 0.12 * std::exp (-t * 18)) / fs;
                y = 0.7 * std::sin (ph) * std::exp (-t * 2.0);
            }
            else
            {
                const double x = (1 - r) * nd (g) + 2 * r * std::cos (w) * s1 - r * r * s2;
                s2 = s1;
                s1 = x;
                ph += 2 * kPi * 185 / fs;
                y = (3.0 * x + 0.4 * std::sin (ph)) * std::exp (-t * 12);
            }
            b[0][a + i] = (float) y;
        }
    }
    return b;
}

// time for a hit's smoothed power to fall 40 dB below its peak
static double t40 (const std::vector<float>& y, size_t from, size_t to, double fs)
{
    double e = 0, pk = 0;
    std::vector<double> ed (to - from);
    for (size_t i = from; i < to; ++i)
    {
        e += 0.005 * ((double) y[i] * y[i] - e);
        ed[i - from] = e;
        pk = std::max (pk, e);
    }
    const size_t ipk = (size_t) (std::max_element (ed.begin(), ed.end()) - ed.begin());
    for (size_t i = ipk; i < ed.size(); ++i)
        if (ed[i] < pk * 1.0e-4)
            return (double) i / fs;
    return (double) ed.size() / fs;
}

//==============================================================================
static void testRate (double fs, bool quick)
{
    const std::string R = " @ " + rateName (fs);
    const char* modeName[3] = { "VOCAL", "SHAPE", "RHYTHM" };
    // 1. silence in -> exactly silence out; NaN / Inf in -> finite out; output = input x gain exactly
    for (int m = 0; m < 3; ++m)
    {
        Params p;
        p.mode = m;
        const auto o = render (fs, p, Buf (2, std::vector<float> ((size_t) fs, 0.0f)), 512, {}, 120.0);
        double mx = 0;
        for (auto& c : o.y)
            for (float v : c)
                mx = std::max (mx, (double) std::abs (v));
        check (std::string ("silence in -> exactly silence out, ") + modeName[m] + R, mx, 0.0, 0.0, "");
        auto v = makeVoice (fs, 3.0);
        v.x[0][(size_t) fs] = std::numeric_limits<float>::quiet_NaN();
        v.x[1][(size_t) fs + 3] = std::numeric_limits<float>::infinity();
        const auto o2 = render (fs, p, v.x, 512, {}, 120.0);
        double bad = 0, err = 0;
        for (size_t i = 0; i < o2.y[0].size(); ++i)
        {
            bad += std::isfinite (o2.y[0][i]) && std::isfinite (o2.y[1][i]) ? 0 : 1;
            const float xin = (i == (size_t) fs) ? 0.0f : v.x[0][i];
            err = std::max (err, (double) std::abs (o2.y[0][i] - xin * o2.g[i]));
        }
        check (std::string ("NaN / Inf in: non-finite output samples, ") + modeName[m] + R, bad, 0, 0, "");
        check (std::string ("output = input (delayed by the latency) x the gate gain, ") + modeName[m] + R, err, 0.0, 1.0e-6, "");
    }
    // 2. block-size independence
    for (int m = 0; m < 3; ++m)
    {
        Params p;
        p.mode = m;
        auto v = makeVoice (fs, 3.0);
        if (m == 1)
            v.x = hits (fs, 0, 3.0, 0.5);
        const auto ref = render (fs, p, v.x, 512, {}, 120.0);
        double d = 0;
        for (int block : { 1, 64, 4096 })
        {
            if (quick && block == 1)
                continue;
            const auto o = render (fs, p, v.x, block, {}, 120.0);
            for (size_t i = 0; i < o.y[0].size(); ++i)
                d = std::max (d, (double) std::abs (o.y[0][i] - ref.y[0][i]));
        }
        check (std::string ("block sizes 1 / 64 / 4096 vs 512: max |difference|, ") + modeName[m] + R, d, 0.0, 1.0e-6, "");
    }
    // 3. VOCAL SMART: open on the voice, closed in the pauses (well away from the phrases); level independence
    for (int speed : { kBest, kFast })
        for (double lvl : { -18.0, -42.0 })
        {
            Params p;
            p.speed = speed;
            auto v = makeVoice (fs, 12.0, lvl, lvl - 48.0);
            const auto o = render (fs, p, v.x);
            double openOnVoice = 0, nv = 0, closedFar = 0, nf = 0;
            const size_t guard = (size_t) (0.15 * fs);
            for (size_t i = (size_t) (2.5 * fs); i < o.g.size(); ++i)
            {
                bool near = false;
                for (size_t k : { i >= guard ? i - guard : 0, std::min (i + guard, o.g.size() - 1) })
                    near = near || v.active[k];
                if (v.active[i])
                {
                    openOnVoice += o.g[i] > 0.5f ? 1 : 0;
                    nv += 1;
                }
                else if (! near)
                {
                    closedFar += o.g[i] < 0.01f ? 1 : 0;
                    nf += 1;
                }
            }
            const std::string sp = speed == kBest ? "BEST" : "FAST";
            char lv[16];
            std::snprintf (lv, sizeof lv, "%.0f dBFS", lvl);
            check ("VOCAL " + sp + ", voice at " + lv + ": gate open during the voice" + R, 100.0 * openOnVoice / nv, 97.0, 100.0, "%");
            check ("VOCAL " + sp + ", voice at " + lv + ": gate closed in the pauses" + R, 100.0 * closedFar / nf, 95.0, 100.0, "%");
            if (speed == kBest && lvl == -18.0)
            {
                // 2.0: 2 s of digital silence before the take (a DAW before the vocal item) must not change that
                const size_t pre = (size_t) (2.0 * fs);
                Buf xs (v.x.size());
                for (size_t c = 0; c < v.x.size(); ++c)
                {
                    xs[c].assign (pre, 0.0f);
                    xs[c].insert (xs[c].end(), v.x[c].begin(), v.x[c].end());
                }
                const auto os = render (fs, p, xs);
                double cf = 0, nn = 0;
                for (size_t i = (size_t) (2.5 * fs); i < v.x[0].size(); ++i)
                {
                    bool near = false;
                    for (size_t k : { i >= guard ? i - guard : 0, std::min (i + guard, v.x[0].size() - 1) })
                        near = near || v.active[k];
                    if (! v.active[i] && ! near)
                    {
                        cf += os.g[i + pre] < 0.01f ? 1 : 0;
                        nn += 1;
                    }
                }
                check ("VOCAL BEST after 2 s of digital silence: gate closed in the pauses" + R, 100.0 * cf / nn, 95.0, 100.0, "%");
            }
        }
    // 4. VOCAL LEVEL (LIVE): zero latency, opens above the threshold, closes below (hysteresis, hold)
    {
        Params p;
        p.speed = kLive;
        p.detect = kLevel;
        p.thresholdDb = -40.0f;
        p.holdMs = 0.0f;
        Buf x (1, std::vector<float> ((size_t) fs, 0.0f));
        for (size_t i = 0; i < x[0].size(); ++i)
            x[0][i] = (float) ((i > (size_t) (0.2 * fs) && i < (size_t) (0.6 * fs) ? 0.1 : 0.001) * std::sin (2 * kPi * 1000.0 * (double) i / fs));
        const auto o = render (fs, p, x);
        check ("LIVE: latency" + R, o.latency, 0, 0, "smp");
        check ("LIVE: open in the loud part (-20 dBFS)" + R, o.g[(size_t) (0.4 * fs)], 0.99, 1.0, "");
        check ("LIVE: closed in the quiet part (-60 dBFS) after the release" + R, db (o.g[(size_t) (0.9 * fs)]), -61.0, -59.0, "dB");
    }
    // 5. SHAPE: tails at TAIL 50 % about half as long; the onset intact; clicks: no new treble on an 808
    for (int kind : { 0, 1, 2 })
    {
        const char* kn = kind == 0 ? "kick" : kind == 1 ? "808" : "snare";
        const double every = kind == 1 ? 3.0 : 1.0; // the 808 rings for seconds
        auto x = hits (fs, kind, 1.2 + 2.0 * every, every);
        Params p;
        p.mode = kShape;
        p.tailPct = 50.0f;
        p.thresholdDb = -60.0f;
        p.holdMs = 0.0f;
        p.releaseMs = 300.0f;
        p.rangeDb = -80.0f;
        Params nat = p;
        nat.tailPct = 100.0f;
        const auto o = render (fs, p, x), on = render (fs, nat, x);
        const size_t a = (size_t) ((0.1 + every) * fs), b = (size_t) ((0.05 + 2.0 * every) * fs);
        const double ratio = t40 (o.y[0], a, b, fs) / t40 (on.y[0], a, b, fs);
        check (std::string ("SHAPE ") + kn + ": tail length at TAIL 50 % re TAIL 100 %" + R, ratio, 0.35, 0.75, "x");
        double eo = 0, ex = 0;
        for (size_t i = a; i < a + (size_t) (0.003 * fs); ++i)
        {
            eo += std::pow ((double) o.y[0][i] - x[0][i], 2);
            ex += (double) x[0][i] * x[0][i];
        }
        check (std::string ("SHAPE ") + kn + ": the first 3 ms of the hit, error re the hit" + R, 10.0 * std::log10 (eo / ex + 1e-30), -300.0, -35.0, "dB");
        if (kind == 1 && fs < 90000.0)
        {
            // treble above 6 kHz in the shaped 808 tail (the 808 itself has none): gain-modulation splatter
            double hf = 0, tot = 0;
            Biquad h1, h2;
            h1.setHighPass (fs, 6000.0);
            h2.setHighPass (fs, 6000.0);
            for (size_t i = a; i < b; ++i)
            {
                const double y = h2.process (h1.process (o.y[0][i], 0), 0);
                if (i > a + (size_t) (0.02 * fs))
                    hf += y * y;
                tot += (double) o.y[0][i] * o.y[0][i];
            }
            check ("SHAPE 808: treble splatter in the shortened tail, re the hit" + R, 10.0 * std::log10 (hf / tot + 1e-30), -300.0, -90.0, "dB");
        }
    }
    // 6. RHYTHM: steps on the host grid, clean edges
    {
        Params p;
        p.mode = kRhythm;
        p.pattern = 0x5555;
        p.rate = 2;
        p.rangeDb = -80.0f;
        p.edgeMs = 3.0f;
        auto x = sine (fs, 220.0, -10.0, 3.0);
        const auto o = render (fs, p, x, 512, {}, 120.0);
        const size_t step = (size_t) std::lround (0.125 * fs);
        double centreErr = 0;
        for (int k = 0; k < 16; ++k)
            centreErr = std::max (centreErr, (double) std::abs (o.g[(size_t) k * step + step / 2] - (k % 2 == 0 ? 1.0f : 0.0001f)));
        check ("RHYTHM: gain at the step centres (open 1, closed -80 dB): max error" + R, centreErr, 0.0, 1.0e-4, "");
        // the 50 % crossing at the start of step 1 (0.125 s): on the grid within a sample
        size_t cross = 0;
        for (size_t i = step - step / 4; i < step + step / 4; ++i)
            if (o.g[i] >= 0.5f && o.g[i + 1] < 0.5f)
                cross = i;
        check ("RHYTHM: closing edge centred on the grid line" + R, ((double) cross + 0.5 - (double) step) / fs * 1000.0, -0.05, 0.05, "ms");
        // swing: every second 16th starts later by swing x half a step
        Params sw = p;
        sw.swingPct = 50.0f;
        sw.pattern = 0x0002; // only step 1 open
        const auto os = render (fs, sw, x, 512, {}, 120.0);
        size_t open = 0;
        for (size_t i = step / 2; i < 2 * step; ++i)
            if (os.g[i] < 0.5f && os.g[i + 1] >= 0.5f)
                open = i;
        check ("RHYTHM: swing 50 % moves the off-beat 16th by a quarter step" + R, ((double) open + 0.5 - (double) step) / (double) step, 0.24, 0.26, "steps");
    }
    // 7. host bypass: exactly the input, delayed
    {
        Params p;
        p.bypass = true;
        auto v = makeVoice (fs, 2.0);
        const auto o = render (fs, p, v.x);
        double err = 0;
        for (size_t i = (size_t) (0.1 * fs); i < o.y[0].size(); ++i)
            err = std::max (err, (double) std::abs (o.y[0][i] - v.x[0][i]));
        check ("host bypass: output = input delayed by the latency" + R, err, 0.0, 0.0, "");
    }
    // 8. BLEED: a reference through a leak path into the mic with a voice on top; the canceller removes it
    {
        const double secs = quick ? 20.0 : 30.0;
        const size_t n = (size_t) (secs * fs);
        std::mt19937 g (4);
        std::normal_distribution<double> nd;
        Buf ref (2, std::vector<float> (n));
        double lp = 0;
        for (size_t i = 0; i < n; ++i)
        {
            lp = 0.7 * lp + 0.3 * nd (g);                      // a busy, coloured "beat"
            const double beatEnv = 0.6 + 0.4 * std::sin (2 * kPi * 2.0 * (double) i / fs);
            ref[0][i] = (float) (0.2 * lp * beatEnv);
            ref[1][i] = (float) (0.2 * lp * beatEnv * 0.9);
        }
        // leak: 1 ms delay, a reflection at 7 ms, -30 dB
        std::vector<float> leak (n, 0.0f);
        const size_t d1 = (size_t) (0.001 * fs), d2 = (size_t) (0.007 * fs);
        for (size_t i = d2; i < n; ++i)
            leak[i] = (float) (0.0316 * (0.6 * (ref[0][i - d1] + ref[1][i - d1]) - 0.25 * (ref[0][i - d2] + ref[1][i - d2])));
        auto v = makeVoice (fs, secs, -18.0, -80.0);
        Buf mic (1, std::vector<float> (n));
        for (size_t i = 0; i < n; ++i)
            mic[0][i] = v.x[0][i] + leak[i];
        Params p;
        p.bleed = true;
        p.rangeDb = 0.0f; // gate fully open: measure the canceller alone
        const auto o = render (fs, p, mic, 512, ref);
        double eIn = 0, eOut = 0, eInV = 0, eOutV = 0;
        for (size_t i = n / 2; i < n; ++i)
        {
            const double res = (double) o.y[0][i] - v.x[0][i];
            (v.active[i] ? eInV : eIn) += (double) leak[i] * leak[i];
            (v.active[i] ? eOutV : eOut) += res * res;
        }
        check ("BLEED: bleed removed in the pauses (second half)" + R, 10.0 * std::log10 (eIn / eOut), 15.0, 200.0, "dB");
        check ("BLEED: bleed removed under the voice (second half)" + R, 10.0 * std::log10 (eInV / eOutV), 10.0, 200.0, "dB");
        // no reference connected: BLEED on does nothing but delay
        const auto o0 = render (fs, p, mic, 512, {});
        double err = 0;
        for (size_t i = 0; i < n; ++i)
            err = std::max (err, (double) std::abs (o0.y[0][i] - mic[0][i] * o0.g[i]));
        check ("BLEED without a side-chain: the input passes unchanged (x gain)" + R, err, 0.0, 1.0e-6, "");

        // 2.0 persistence: what was learned is saved with the project and restored, and survives a reset()
        auto run = [&] (Engine& e, size_t len) {
            const int L = e.getLatencySamples();
            Buf x (1, std::vector<float> (len + (size_t) L, 0.0f)), r (2, std::vector<float> (len + (size_t) L, 0.0f));
            for (size_t i = 0; i < len; ++i)
            {
                x[0][i] = mic[0][i];
                r[0][i] = ref[0][i];
                r[1][i] = ref[1][i];
            }
            for (size_t pos = 0; pos < x[0].size(); pos += 512)
            {
                const int m = (int) std::min<size_t> (512, x[0].size() - pos);
                float* io[1] = { x[0].data() + pos };
                const float* q[2] = { r[0].data() + pos, r[1].data() + pos };
                e.process (io, 1, m, q, 2);
            }
            x[0].erase (x[0].begin(), x[0].begin() + L);
            return x[0];
        };
        auto firstSecond = [&] (const std::vector<float>& y) { // bleed removed in the pauses of the first phrase (0.1-2.4 s)
            double a = 0, b = 0;
            for (size_t i = (size_t) (0.1 * fs); i < (size_t) (2.4 * fs); ++i)
                if (! v.active[i])
                {
                    const double res = (double) y[i] - v.x[0][i];
                    a += (double) leak[i] * leak[i];
                    b += res * res;
                }
            return 10.0 * std::log10 (a / (b + 1e-30));
        };
        Engine e1;
        e1.setParams (p);
        e1.prepare (fs, 1, 512);
        run (e1, n);
        std::vector<float> path;
        double pathRate = 0;
        const bool got = e1.copyBleedPath (path, pathRate);
        check ("BLEED path: available after learning" + R, got ? 1 : 0, 1, 1, "");
        check ("BLEED meter: the gaps are quieter by" + R, e1.bleedGapDb(), 10.0, 200.0, "dB");
        check ("BLEED meter: fit of the estimate" + R, e1.bleedFit(), 0.8, 1.0, "");
        check ("BLEED meter: never reads more than the true removal (meter - measured)" + R, e1.bleedGapDb() - 10.0 * std::log10 (eIn / eOut), -100.0, 0.0, "dB");
        Engine fresh, restored;
        for (auto* e : { &fresh, &restored })
        {
            e->setParams (p);
            e->prepare (fs, 1, 512);
        }
        check ("BLEED path: accepted at the same sample rate" + R, restored.setBleedPath (path.data(), (int) path.size(), pathRate) ? 1 : 0, 1, 1, "");
        const double f0 = firstSecond (run (fresh, (size_t) (2.4 * fs))), f1 = firstSecond (run (restored, (size_t) (2.4 * fs)));
        check ("BLEED restored: removed in the first phrase's pauses (fresh: " + std::to_string ((int) std::lround (f0)) + " dB)" + R, f1, 15.0, 200.0, "dB");
        e1.reset();
        check ("BLEED after reset(): removed in the first phrase's pauses" + R, firstSecond (run (e1, (size_t) (2.4 * fs))), 15.0, 200.0, "dB");
        // the take ends, the beat plays on (mic digital silence): silence out, and nothing unlearned
        {
            const int L = e1.getLatencySamples();
            const size_t m = (size_t) (3.0 * fs) + (size_t) L;
            std::vector<float> z (m, 0.0f);
            double mx = 0;
            for (size_t pos = 0; pos < m; pos += 512)
            {
                const int len = (int) std::min<size_t> (512, m - pos);
                float* io[1] = { z.data() + pos };
                const size_t at = pos % (n - 600); // the beat continues
                const float* q[2] = { ref[0].data() + at, ref[1].data() + at };
                e1.process (io, 1, len, q, 2);
                for (int i = 0; i < len; ++i)
                    if (pos + (size_t) i >= (size_t) L)
                        mx = std::max (mx, (double) std::abs (z[pos + (size_t) i]));
            }
            check ("BLEED: mic silent while the beat plays -> exactly silence out" + R, mx, 0.0, 0.0, "");
            e1.reset();
            check ("BLEED: after the silence, removed in the first phrase's pauses" + R, firstSecond (run (e1, (size_t) (2.4 * fs))), 15.0, 200.0, "dB");
        }
        Engine other;
        other.setParams (p);
        other.prepare (fs == 48000.0 ? 44100.0 : 48000.0, 1, 512);
        check ("BLEED path: refused at another sample rate" + R, other.setBleedPath (path.data(), (int) path.size(), pathRate) ? 1 : 0, 0, 0, "");
        auto bad = path;
        bad[bad.size() / 3] = std::numeric_limits<float>::quiet_NaN();
        Engine corrupt;
        corrupt.setParams (p);
        corrupt.prepare (fs, 1, 512);
        corrupt.setBleedPath (bad.data(), (int) bad.size(), pathRate);
        {
            std::vector<float> z (64, 0.0f);
            float* io[1] = { z.data() };
            corrupt.process (io, 1, 64); // the handoff happens here; a rejected path publishes nothing
        }
        const bool ignored = ! corrupt.bleedLearned();
        const auto yc = run (corrupt, (size_t) (0.2 * fs));
        bool finite = true;
        for (float y : yc)
            finite = finite && std::isfinite (y);
        check ("BLEED path: a corrupt path is ignored (output finite)" + R, finite && ignored ? 1 : 0, 1, 1, "");
    }
    // 9. every factory preset: finite, no gain on the open voice (unity), no overload
    {
        auto v = makeVoice (fs, 4.0);
        int k = 0;
        for (const auto& pr : factoryPresets())
        {
            if (quick && k++ % 3 != 0)
                continue;
            const auto o = render (fs, pr.params, v.x, 512, {}, 120.0);
            double mx = 0, pkIn = 0;
            bool fin = true;
            for (size_t i = 0; i < o.y[0].size(); ++i)
            {
                fin = fin && std::isfinite (o.y[0][i]);
                mx = std::max (mx, (double) std::abs (o.y[0][i]));
                pkIn = std::max (pkIn, (double) std::abs (v.x[0][i]));
            }
            check (std::string ("preset '") + pr.name + "': output peak re input peak (finite: " + (fin ? "yes" : "NO") + ")" + R,
                   fin ? db (mx / pkIn) : 99.0, -80.0, 0.01, "dB");
        }
    }
}

// mode switches while playing: no click, the latency follows (offline: at once)
// 2.0: SHAPE -> MIDI, RHYTHM step levels, VOCAL LEVEL key filters and listen
static void testV2 (double fs)
{
    const std::string R = " @ " + rateName (fs);
    // MIDI: one note per hit, at the hit as heard, velocity from its peak; every note ends
    {
        auto x = hits (fs, 0, 4.0, 0.5);
        for (int h = 1; h < 7; h += 2) // every second kick 20 dB down
            for (size_t i = (size_t) ((0.1 + 0.5 * h) * fs); i < (size_t) ((0.58 + 0.5 * h) * fs); ++i)
                x[0][i] *= 0.1f;
        Params p;
        p.mode = kShape;
        p.thresholdDb = -50.0f;
        p.tailPct = 50.0f;
        Engine e;
        e.setParams (p);
        e.prepare (fs, 1, 512);
        const int L = e.getLatencySamples();
        x[0].resize (x[0].size() + (size_t) L + (size_t) (0.1 * fs), 0.0f);
        std::vector<std::pair<double, int>> on, off; // (output time re input, velocity / note)
        bool noteSwitched = false;
        int switchedNoteOffs = 0;
        for (size_t pos = 0; pos < x[0].size(); pos += 512)
        {
            if (! noteSwitched && pos >= (size_t) (3.12 * fs)) // change the note while the last hit's note sounds
            {
                p.midiNote = 40;
                e.setParams (p);
                noteSwitched = true;
            }
            const int m = (int) std::min<size_t> (512, x[0].size() - pos);
            float* io[1] = { x[0].data() + pos };
            e.process (io, 1, m);
            const MidiEvent* ev = nullptr;
            const int ne = e.midiEvents (ev);
            for (int k = 0; k < ne; ++k)
            {
                const double t = ((double) pos + ev[k].offset - L) / fs;
                if (ev[k].velocity > 0)
                    on.push_back ({ t, ev[k].velocity });
                else
                {
                    off.push_back ({ t, ev[k].note });
                    switchedNoteOffs += noteSwitched && ev[k].note == 36 ? 1 : 0;
                }
            }
        }
        check ("SHAPE MIDI: one note per hit" + R, (double) on.size(), 7, 7, "notes");
        double worst = 0;
        for (size_t k = 0; k < on.size(); ++k)
            worst = std::max (worst, std::abs (on[k].first - (0.1 + 0.5 * (double) k)));
        check ("SHAPE MIDI: note on at the hit (worst)" + R, worst * 1000.0, 0.0, 3.0, "ms");
        int minLoud = 127, maxQuiet = 0;
        for (size_t k = 0; k < on.size(); ++k)
            (k % 2 == 0 ? minLoud : maxQuiet) = k % 2 == 0 ? std::min (minLoud, on[k].second) : std::max (maxQuiet, on[k].second);
        check ("SHAPE MIDI: velocity, loud hits minus 20 dB quieter hits" + R, minLoud - maxQuiet, 40, 60, "");
        check ("SHAPE MIDI: every note ends" + R, (double) off.size(), (double) on.size(), (double) on.size(), "offs");
        check ("SHAPE MIDI: a note ends with the number it started with (note changed while sounding)" + R, switchedNoteOffs, 1, 1, "");
    }
    // RHYTHM: the level of each open step
    {
        Params p;
        p.mode = kRhythm;
        p.pattern = 0xFFFFu;
        p.rate = 2;
        p.rangeDb = -80.0f;
        const float lv[4] = { 1.0f, 0.5f, 0.25f, 0.0f };
        for (int k = 0; k < 16; ++k)
            p.stepLevel[(size_t) k] = lv[k % 4];
        Buf dc (1, std::vector<float> ((size_t) (2.0 * fs), 0.5f));
        const auto o = render (fs, p, dc, 512, {}, 120.0);
        const double step = 60.0 / 120.0 / 4.0 * fs;
        const double floorLin = std::pow (10.0, -80.0 / 20.0);
        double err = 0;
        for (int k = 0; k < 16; ++k)
        {
            const double want = floorLin + (1.0 - floorLin) * lv[k % 4];
            err = std::max (err, std::abs ((double) o.g[(size_t) ((k + 0.5) * step)] - want));
        }
        check ("RHYTHM step levels: gain at the step centres, max error" + R, err, 0.0, 1.0e-4, "");
    }
    // VOCAL LEVEL key: high-pass, low-pass, listen
    {
        auto openFrac = [&] (const Params& p, const Buf& x) {
            const auto o = render (fs, p, x, 512);
            int open = 0, cnt = 0;
            for (size_t i = (size_t) (0.3 * fs); i < o.g.size(); ++i, ++cnt)
                open += o.g[i] > 0.5f ? 1 : 0;
            return 100.0 * open / std::max (1, cnt);
        };
        Params p;
        p.detect = kLevel;
        p.speed = kLive;
        p.thresholdDb = -45.0f;
        const auto lo = sine (fs, 60.0, -20.0, 1.0), hi = sine (fs, std::min (8000.0, 0.4 * fs), -20.0, 1.0);
        check ("LEVEL key: 60 Hz at -20 dBFS opens with the default key (80 Hz high-pass)" + R, openFrac (p, lo), 99.0, 100.0, "%");
        p.keyHpHz = 200.0f;
        check ("LEVEL key: ... stays closed with KEY HP 200 Hz" + R, openFrac (p, lo), 0.0, 0.0, "%");
        p.keyHpHz = 80.0f;
        check ("LEVEL key: 8 kHz at -20 dBFS opens with KEY LP off" + R, openFrac (p, hi), 99.0, 100.0, "%");
        p.keyLpHz = 1000.0f;
        check ("LEVEL key: ... stays closed with KEY LP 1 kHz" + R, openFrac (p, hi), 0.0, 0.0, "%");
        p.keyLpHz = 20000.0f;
        p.keyHpHz = 1000.0f;
        p.keyListen = true;
        const auto o = render (fs, p, sine (fs, 100.0, -20.0, 1.0), 512);
        double e = 0;
        for (size_t i = (size_t) (0.3 * fs); i < o.y[0].size(); ++i)
            e += (double) o.y[0][i] * o.y[0][i];
        const double inE = 0.5 * std::pow (10.0, -20.0 / 10.0) * (double) (o.y[0].size() - (size_t) (0.3 * fs));
        check ("LEVEL KEY LISTEN: a 100 Hz tone through KEY HP 1 kHz, level re input" + R, 10.0 * std::log10 (e / inE + 1e-30), -300.0, -40.0, "dB");
    }
}

// 2.0: the room model, the pause model, and the gate in a reverberant room
static void testRoom (double fs)
{
    const std::string R = " @ " + rateName (fs);
    std::mt19937 g (21);
    std::normal_distribution<double> nd;
    // a room with RT60 0.5 s: noise bursts (200 ms) ringing out, then silence; the model must learn the room
    {
        const double rt = 0.5;
        const size_t n = (size_t) (20.0 * fs);
        std::vector<float> x (n, 0.0f);
        double tail = 0.0;
        const double k = std::exp (-6.91 / (rt * fs)); // amplitude decay per sample (-60 dB in rt)
        for (size_t i = 0; i < n; ++i)
        {
            const double t = std::fmod ((double) i / fs, 1.0);
            const double src = t < 0.2 ? 0.3 * nd (g) : 0.0;
            tail = tail * k + 0.02 * src;            // a diffuse field: decays at the room's rate
            x[i] = (float) (src + 20.0 * tail * nd (g) * 0.05 + 1.0e-5 * nd (g));
        }
        RoomModel rm;
        const int hop = (int) std::lround (0.002 * fs);
        rm.prepare (fs, hop);
        double exTail = 0, exOn = 0;
        int nTail = 0, nOn = 0;
        for (size_t i = 0; i < n; ++i)
        {
            rm.push (x[i]);
            if ((i + 1) % (size_t) hop == 0 && i > (size_t) (10.0 * fs))
            {
                const double t = std::fmod ((double) i / fs, 1.0);
                const auto o = rm.at (0);
                if (t > 0.30 && t < 0.45) { exTail += o.excess; ++nTail; }
                if (t > 0.003 && t < 0.02) { exOn += o.excess; ++nOn; }
            }
        }
        const double est = (rm.bandRt60 (1) + rm.bandRt60 (2)) / 2.0;
        check ("room model: learns RT60 0.5 s (mean of the mid bands)" + R, est, 0.38, 0.65, "s");
        check ("room model: a pure tail is explained by the room (unexplained share)" + R, exTail / std::max (1, nTail), 0.0, 0.2, "");
        check ("room model: an onset is not (unexplained share)" + R, exOn / std::max (1, nOn), 0.6, 1.0, "");
    }
    // the pause model: after 50 ms of pause, middling evidence reopens; after 600 ms it does not
    {
        auto after = [] (int pauseFrames) {
            FixedLagHmm h;
            h.setup (0, 0.998, 0.995, 0.75, 0.03, 0.012);
            h.reset();
            double post = 0;
            for (int i = 0; i < 200; ++i) h.push (0.97, post);
            for (int i = 0; i < pauseFrames; ++i) h.push (0.05, post);
            for (int i = 0; i < 5; ++i) h.push (0.8, post);
            return post;
        };
        const double shortP = after (25), longP = after (300);
        check ("pause model: a release after a 50 ms closure vs the same evidence after 600 ms of pause (P(voice) ratio)" + R,
               shortP / std::max (1.0e-9, longP), 3.0, 1.0e9, "x");
    }
    // the gate in a reverberant room (RT60 0.5 s on the voice): open on the voice, closed in the pauses
    {
        auto v = makeVoice (fs, 14.0, -18.0, -70.0);
        const size_t n = v.x[0].size();
        const double k = std::exp (-6.91 / (0.5 * fs));
        // a diffuse tail: noise with the voice's energy envelope decayed at the room's rate, 10 dB under the voice
        double env = 0.0;
        std::vector<float> rev (n);
        for (size_t i = 0; i < n; ++i)
        {
            env = env * k * k + (double) v.x[0][i] * v.x[0][i] * (1.0 - k * k);
            rev[i] = (float) (v.x[0][i] + 0.32 * std::sqrt (env) * nd (g));
        }
        Buf x (1, rev);
        Params p;
        const auto o = render (fs, p, x);
        double openOnVoice = 0, nv = 0, closedFar = 0, nf = 0;
        const size_t guard = (size_t) (0.25 * fs);
        for (size_t i = (size_t) (2.5 * fs); i < o.g.size(); ++i)
        {
            bool near = false;
            for (size_t j : { i >= guard ? i - guard : 0, std::min (i + guard, o.g.size() - 1) })
                near = near || v.active[j];
            if (v.active[i]) { openOnVoice += o.g[i] > 0.5f ? 1 : 0; nv += 1; }
            else if (! near) { closedFar += o.g[i] < 0.01f ? 1 : 0; nf += 1; }
        }
        check ("VOCAL BEST in a reverberant room (RT60 0.5 s): open during the voice" + R, 100.0 * openOnVoice / nv, 97.0, 100.0, "%");
        check ("VOCAL BEST in a reverberant room: closed in the pauses (250 ms from the voice)" + R, 100.0 * closedFar / nf, 95.0, 100.0, "%");
    }
}

static void testSwitching()
{
    const double fs = 48000.0;
    auto v = makeVoice (fs, 6.0);
    Params p;
    Engine e;
    e.setParams (p);
    e.prepare (fs, 2, 256);
    Buf y = v.x;
    float maxIn = 0.0f, maxOut = 0.0f;
    for (size_t i = 1; i < v.x[0].size(); ++i)
        maxIn = std::max (maxIn, std::abs (v.x[0][i] - v.x[0][i - 1]));
    for (size_t s = 0, k = 0; s < y[0].size(); s += 256, ++k)
    {
        if (k % 150 == 75)
        {
            p.mode = (p.mode + 1) % 3;
            e.setParams (p);
        }
        float* io[2] = { y[0].data() + s, y[1].data() + s };
        e.process (io, 2, (int) std::min<size_t> (256, y[0].size() - s));
    }
    for (size_t i = 1; i < y[0].size(); ++i)
        maxOut = std::max (maxOut, std::abs (y[0][i] - y[0][i - 1]));
    check ("mode switches while playing: largest sample step re the input's", maxOut / maxIn, 0.0, 1.5, "x");
}

static void testAllocAndCpu (bool quick)
{
    {
        const double fs = 48000.0;
        auto v = makeVoice (fs, 4.0);
        Params p;
        p.bleed = true;
        Engine e;
        e.setParams (p);
        e.prepare (fs, 2, 512);
        allocCount = 0;
        Buf y = v.x, sc = v.x;
        for (size_t s = 0, k = 0; s + 512 <= y[0].size(); s += 512, ++k)
        {
            p.mode = (int) (k / 60) % 3;
            p.speed = (int) (k / 25) % 3;
            p.detect = (int) (k / 40) % 2;
            p.bypass = k % 70 > 64;
            p.tailPct = (float) (20 + k % 80);
            e.setParams (p);
            Transport t;
            t.valid = t.playing = true;
            t.ppq = (double) s / fs * 2.0;
            e.setTransport (t);
            float* io[2] = { y[0].data() + s, y[1].data() + s };
            const float* q[2] = { sc[0].data() + s, sc[1].data() + s };
            countAllocs = true;
            e.process (io, 2, 512, q, 2);
            countAllocs = false;
        }
        check ("no memory allocation in process() (mode / speed / detection switches, bypass, BLEED)", (double) allocCount, 0, 0, "allocs");
    }
    if (quick)
        return;
    struct Case { const char* name; Params p; bool sc; };
    Params vocal, bleedP, shapeP, rhythmP;
    bleedP.bleed = true;
    shapeP.mode = kShape;
    rhythmP.mode = kRhythm;
    for (auto& c : { Case { "VOCAL smart BEST", vocal, false }, Case { "VOCAL smart + BLEED", bleedP, true }, Case { "SHAPE", shapeP, false },
                     Case { "RHYTHM", rhythmP, false } })
    {
        const double fs = 48000.0;
        auto v = makeVoice (fs, 10.0);
        Engine e;
        e.setParams (c.p);
        e.prepare (fs, 2, 512);
        Buf y = v.x, sc = v.x;
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t s = 0; s + 512 <= y[0].size(); s += 512)
        {
            float* io[2] = { y[0].data() + s, y[1].data() + s };
            const float* q[2] = { sc[0].data() + s, sc[1].data() + s };
            e.process (io, 2, 512, q, c.sc ? 2 : 0);
        }
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        check (std::string ("CPU, 48 kHz stereo, ") + c.name + " (% of one core)", 100.0 * sec / 10.0, 0.0, 5.0, "%");
    }
}

int main (int argc, char** argv)
{
    const bool quick = argc > 1 && std::strcmp (argv[1], "--quick") == 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (double fs : quick ? std::vector<double> { 48000.0 } : std::vector<double> { 44100.0, 48000.0, 96000.0 })
    {
        testRate (fs, quick);
        testV2 (fs);
        testRoom (fs);
    }
    testSwitching();
    testAllocAndCpu (quick);
    const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    if (fails == 0)
        std::printf ("ALL PASS: %d of %d checks passed (%.1f s)\n", checks, checks, sec);
    else
        std::printf ("FAILED: %d of %d checks (%.1f s)\n", fails, checks, sec);
    return fails;
}
