// Axell Gate - plugin-level tests (JUCE): parameters, state, latency reporting, presets, A/B, side-chain bus, bypass,
// mono, editor.
// Exit code = number of failures. Registered with CTest as "plugin_suite".
#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

#include <cstdio>
#include <random>
#include <thread>

namespace
{
int failures = 0, checks = 0;
using Buf = std::vector<std::vector<float>>;

void check (bool ok, const juce::String& what)
{
    ++checks;
    if (! ok)
        ++failures;
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
}

void setParam (GateProcessor& p, const char* id, float value)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}
float getParam (GateProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); }

void prepare (GateProcessor& p, double fs = 48000.0, int block = 256, int ch = 2)
{
    p.setPlayConfigDetails (ch, ch, fs, block);
    p.prepareToPlay (fs, block);
}

// a voice-like test signal: vowels (pulse train through formants) and noise sibilants, gain in dB
Buf voice (int n, double fs, double gainDb, unsigned seed)
{
    Buf b (2, std::vector<float> ((size_t) n));
    std::mt19937 g (seed);
    std::normal_distribution<double> nd;
    const double pi = juce::MathConstants<double>::pi, gain = std::pow (10.0, gainDb / 20.0);
    double ph = 0, s1 = 0, s2 = 0;
    const double w = 2 * pi * 7000 / fs, r = std::exp (-pi * 1500 / fs);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs, u = std::fmod (t, 0.42);
        double y = 0;
        if (u < 0.3)
        {
            ph += 2 * pi * 140 / fs;
            for (int h = 1; h < 30; ++h)
                y += (std::exp (-std::pow ((h * 140.0 - 650) / 250, 2)) + 0.5 * std::exp (-std::pow ((h * 140.0 - 1150) / 300, 2))) * std::sin (h * ph) / h;
            y *= 0.3 * std::min (1.0, std::min (u, 0.3 - u) / 0.02);
        }
        const double a = (1 - r) * nd (g) + 2 * r * std::cos (w) * s1 - r * r * s2;
        s2 = s1;
        s1 = a;
        if (u >= 0.3)
            y += 0.6 * a * std::sin (pi * (u - 0.3) / 0.12);
        b[0][(size_t) i] = b[1][(size_t) i] = (float) (gain * y);
    }
    return b;
}

Buf render (GateProcessor& p, const Buf& in, int block = 256, bool bypassed = false)
{
    const int nc = p.getTotalNumOutputChannels();
    Buf out ((size_t) nc, std::vector<float> (in[0].size()));
    juce::AudioBuffer<float> buf (nc, block);
    juce::MidiBuffer midi;
    for (size_t pos = 0; pos + (size_t) block <= in[0].size(); pos += (size_t) block)
    {
        for (int c = 0; c < nc; ++c)
            buf.copyFrom (c, 0, in[(size_t) c].data() + pos, block);
        if (bypassed)
            p.processBlockBypassed (buf, midi);
        else
            p.processBlock (buf, midi);
        for (int c = 0; c < nc; ++c)
            std::copy (buf.getReadPointer (c), buf.getReadPointer (c) + block, out[(size_t) c].begin() + (long) pos);
    }
    return out;
}

void pumpMessages (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;

    // user presets go to a temporary XDG data dir, never the real one
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("axell-gate-plugin-tests");
    tmp.deleteRecursively();
    tmp.createDirectory();
    setenv ("XDG_DATA_HOME", tmp.getFullPathName().toRawUTF8(), 1);

    //== 1. frozen parameter IDs and order (automation uses the index), version hints, defaults
    {
        GateProcessor p;
        juce::StringArray frozen { "mode", "detect", "speed", "threshold", "sensitivity", "breath", "bleed", "bleedFreeze", "range", "attack",
                                   "hold", "release", "tail", "sensDb", "rate", "steps", "swing", "edge", "output" };
        for (int k = 1; k <= 16; ++k)
            frozen.add ("step" + juce::String (k));
        const int v1Count = frozen.size();
        frozen.add ("midiNote"); // 2.0, appended
        for (int k = 1; k <= 16; ++k)
            frozen.add ("level" + juce::String (k));
        frozen.addArray (juce::StringArray { "keyHp", "keyLp", "keyListen" });
        auto& params = p.getParameters();
        check (params.size() == frozen.size(), "parameter count = " + juce::String (frozen.size()) + " (1.0: " + juce::String (v1Count) + ")");
        for (int i = 0; i < params.size() && i < frozen.size(); ++i)
        {
            auto* wp = dynamic_cast<juce::AudioProcessorParameterWithID*> (params[i]);
            const int hint = i < v1Count ? 1 : 2;
            check (wp != nullptr && wp->getParameterID() == frozen[i] && wp->getVersionHint() == hint,
                   "parameter index " + juce::String (i) + " is '" + frozen[i] + "' (version hint " + juce::String (hint) + ")");
        }
        check (p.producesMidi() && ! p.acceptsMidi(), "MIDI: produces (SHAPE hits), accepts none");
        const auto d = p.readParams();
        check (d.mode == axg::kVocal && d.detect == axg::kSmart && d.speed == axg::kBest && std::abs (d.rangeDb + 60.0f) < 1e-4f
                   && std::abs (d.breathDb + 12.0f) < 1e-4f && ! d.bleed && d.pattern == 0x5555u && d.steps == 16,
               "defaults: VOCAL smart BEST, range -60 dB, breaths -12 dB, bleed off, pattern on the beats");
    }

    //== 2. state save/restore: non-default values -> identical parameters + identical audio
    {
        GateProcessor a;
        const std::pair<const char*, float> values[] = { { "mode", 0.0f }, { "detect", 0.0f }, { "speed", 1.0f }, { "threshold", -33.0f },
                                                         { "sensitivity", 25.0f }, { "breath", -20.0f }, { "bleed", 0.0f }, { "range", -45.0f },
                                                         { "attack", 3.5f }, { "hold", 120.0f }, { "release", 90.0f }, { "tail", 60.0f },
                                                         { "sensDb", 12.0f }, { "rate", 3.0f }, { "steps", 12.0f }, { "swing", 30.0f },
                                                         { "edge", 5.0f }, { "output", -1.5f }, { "step2", 1.0f }, { "step3", 0.0f } };
        for (auto& v : values)
            setParam (a, v.first, v.second);
        a.setPresetName ("My Lead Vocal");
        juce::MemoryBlock state;
        a.getStateInformation (state);

        GateProcessor b;
        b.setStateInformation (state.getData(), (int) state.getSize());
        bool same = true;
        for (auto& v : values)
            if (std::abs (getParam (a, v.first) - getParam (b, v.first)) > 1.0e-6f)
            {
                same = false;
                std::printf ("   mismatch %s: %f vs %f\n", v.first, getParam (a, v.first), getParam (b, v.first));
            }
        check (same, "state restore: all parameter values identical");
        check (b.getPresetName() == "My Lead Vocal", "state restore: preset name restored");
        prepare (a);
        prepare (b);
        const auto sig = voice (48000 * 2, 48000.0, -16.0, 5);
        const auto oa = render (a, sig);
        const auto ob = render (b, sig);
        double diff = 0.0;
        for (int c = 0; c < 2; ++c)
            for (size_t i = 0; i < oa[0].size(); ++i)
                diff = std::max (diff, (double) std::abs (oa[(size_t) c][i] - ob[(size_t) c][i]));
        check (diff <= 0.0, "state restore: restored instance renders bit-identical audio (max diff " + juce::String (diff) + ")");
        check (b.getLatencySamples() == a.getLatencySamples() && a.getLatencySamples() == a.engine().latencyOf ({ axg::kVocal, axg::kFast, true, false }),
               "state restore: latency = VOCAL FAST (" + juce::String (b.getLatencySamples()) + " samples)");
    }

    //== 2b. pluginval-style: raw (unsnapped) normalised values on every parameter, save, scramble, restore
    {
        juce::Random rng (1234);
        bool ok = true;
        for (int round = 0; round < 50 && ok; ++round)
        {
            GateProcessor p;
            auto& params = p.getParameters();
            for (auto* prm : params)
                prm->setValueNotifyingHost (rng.nextFloat());
            std::vector<float> saved;
            for (auto* prm : params)
                saved.push_back (prm->getValue());
            juce::MemoryBlock mb;
            p.getStateInformation (mb);
            for (auto* prm : params)
                prm->setValueNotifyingHost (rng.nextFloat());
            p.setStateInformation (mb.getData(), (int) mb.getSize());
            for (int i = 0; i < params.size(); ++i)
            {
                auto* rp = dynamic_cast<juce::RangedAudioParameter*> (params[i]);
                const float expected = rp->convertTo0to1 (rp->convertFrom0to1 (saved[(size_t) i]));
                if (std::abs (params[i]->getValue() - expected) > 0.01f)
                {
                    ok = false;
                    std::printf ("   round %d %s: %f expected %f\n", round, rp->paramID.toRawUTF8(), params[i]->getValue(), expected);
                }
            }
        }
        check (ok, "raw-value state restore (pluginval scenario), 50 random rounds");
    }

    //== 3. latency reporting follows mode / speed / bleed (handshake + message-thread timer)
    {
        GateProcessor p;
        prepare (p);
        check (p.getLatencySamples() == p.engine().latencyOf ({ axg::kVocal, axg::kBest, true, false }),
               "prepare: latency = VOCAL BEST (" + juce::String (p.getLatencySamples()) + ")");
        const auto sig = voice (9600, 48000.0, -16.0, 1);
        bool finite = true;
        struct Step { int mode, speed; bool bleed; };
        for (auto st : { Step { 0, 1, false }, Step { 0, 0, false }, Step { 1, 2, false }, Step { 2, 2, false }, Step { 0, 2, true } })
        {
            setParam (p, "mode", (float) st.mode);
            setParam (p, "speed", (float) st.speed);
            setParam (p, "bleed", st.bleed ? 1.0f : 0.0f);
            for (int k = 0; k < 4; ++k)
            {
                const auto out = render (p, sig);
                for (auto& ch : out)
                    for (float v : ch)
                        finite = finite && std::isfinite (v) && std::abs (v) < 2.0f;
                pumpMessages (60);
            }
            axg::Engine::Config c;
            c.mode = st.mode;
            c.speed = st.mode == axg::kVocal ? st.speed : axg::kLive;
            c.smart = st.mode == axg::kVocal && st.speed != axg::kLive;
            c.bleed = st.mode == axg::kVocal && st.bleed;
            const int expect = p.engine().latencyOf (c);
            check (p.getLatencySamples() == expect && p.engine().getLatencySamples() == expect,
                   "mode " + juce::String (st.mode) + " speed " + juce::String (st.speed) + (st.bleed ? " + BLEED" : "") + ": reported latency "
                       + juce::String (p.getLatencySamples()) + " = " + juce::String (expect));
        }
        check (finite, "every sample during the switches finite and in range");
    }

    //== 4. factory programs
    {
        GateProcessor p;
        const auto list = axg::factoryPresets();
        check (p.getNumPrograms() == 25, "programs: " + juce::String (p.getNumPrograms()) + " factory presets (1.0: 21, numbers kept)");
        bool allOk = true;
        for (int i = 0; i < p.getNumPrograms(); ++i)
        {
            p.setCurrentProgram (i);
            const auto r = p.readParams();
            const auto& e = list[(size_t) i].params;
            if (r.mode != e.mode || r.detect != e.detect || r.speed != e.speed || std::abs (r.rangeDb - e.rangeDb) > 0.051f
                || std::abs (r.breathDb - e.breathDb) > 0.051f || r.bleed != e.bleed || std::abs (r.tailPct - e.tailPct) > 0.051f
                || r.pattern != e.pattern || r.rate != e.rate || r.steps != e.steps || std::abs (r.swingPct - e.swingPct) > 0.51f
                || std::abs (r.holdMs - e.holdMs) > 0.051f || std::abs (r.releaseMs - e.releaseMs) > 0.051f
                || r.midiNote != e.midiNote || std::abs (r.keyHpHz - e.keyHpHz) > 0.51f || std::abs (r.keyLpHz - e.keyLpHz) > 5.1f
                || [&] { for (int k = 0; k < 16; ++k) if (std::abs (r.stepLevel[(size_t) k] - e.stepLevel[(size_t) k]) > 0.0011f) return true; return false; }())
            {
                allOk = false;
                std::printf ("   program %d (%s) did not apply\n", i, list[(size_t) i].name);
            }
        }
        check (allOk, "every program applies its values");
        p.setCurrentProgram (0);
        bool defaults = true;
        for (auto* prm : p.getParameters())
            if (std::abs (prm->getValue() - prm->getDefaultValue()) > 1.0e-4f)
            {
                defaults = false;
                std::printf ("   Init: %s not default\n", prm->getName (40).toRawUTF8());
            }
        check (defaults, "'Init' program = all parameter defaults");
        std::thread t ([&p] { p.setCurrentProgram (9); });
        t.join();
        check (p.getCurrentProgram() == 9 && p.readParams().mode == list[9].params.mode, "program change from another thread applies the values");
        pumpMessages (150);
        check (p.getPresetName() == list[9].name, "program change from another thread: name updated by the timer");
        const juce::StringArray brands { "fabfilter", "pro-g", "sonnox", "oxford", "sonible", "drawmer", "ssl", "neve", "api", "waves", "polyverse", "gatekeeper", "hush" };
        bool brand = false;
        for (const auto& pr : list)
        {
            juce::StringArray words;
            words.addTokens (juce::String (pr.name) + " " + pr.description, " -,.:;()/'", "");
            for (const auto& w : words)
                if (brands.contains (w.toLowerCase()))
                {
                    brand = true;
                    std::printf ("   brand word '%s' in '%s'\n", w.toRawUTF8(), pr.name);
                }
        }
        check (! brand, "no third-party brand or product names in preset names or descriptions");
    }

    //== 5. user presets save/load
    {
        GateProcessor p;
        setParam (p, "range", -33.0f);
        setParam (p, "mode", 1.0f);
        check (p.saveUserPreset ("Test / Vocal: 1"), "user preset saved (name with illegal file characters)");
        auto files = GateProcessor::userPresetFolder().findChildFiles (juce::File::findFiles, false, "*.axgpreset");
        check (files.size() == 1 && GateProcessor::userPresetFolder().isAChildOf (tmp), "user preset written to $XDG_DATA_HOME/AxellGate/Presets");
        GateProcessor q;
        check (files.size() == 1 && q.loadUserPreset (files[0]), "user preset loads");
        check (std::abs (getParam (q, "range") + 33.0f) < 1e-4f && std::abs (getParam (q, "mode") - 1.0f) < 1e-4f
                   && q.getPresetName() == "Test / Vocal: 1",
               "user preset restores values and name");
    }

    //== 6. A/B compare
    {
        GateProcessor p;
        setParam (p, "tail", 30.0f);
        p.selectSlot (1);
        setParam (p, "tail", 80.0f);
        p.selectSlot (0);
        const float a = getParam (p, "tail");
        p.selectSlot (1);
        const float b = getParam (p, "tail");
        check (std::abs (a - 30.0f) < 1e-3f && std::abs (b - 80.0f) < 1e-3f, "A/B: each slot keeps its own settings");
        p.copyActiveToOther();
        p.selectSlot (0);
        check (std::abs (getParam (p, "tail") - 80.0f) < 1e-3f, "A/B: copy B > A");
    }

    //== 7. side-chain bus: optional, mono or stereo; with the beat on it BLEED runs
    {
        GateProcessor p;
        auto layout = p.getBusesLayout();
        check (layout.inputBuses.size() == 2 && layout.inputBuses[1].isDisabled(), "side-chain bus present and disabled by default");
        layout.inputBuses.getReference (1) = juce::AudioChannelSet::stereo();
        check (p.setBusesLayout (layout), "stereo side-chain accepted");
        p.setRateAndBufferSizeDetails (48000.0, 256); // keeps the bus layout
        p.prepareToPlay (48000.0, 256);
        setParam (p, "bleed", 1.0f);
        juce::AudioBuffer<float> buf (4, 256);
        juce::MidiBuffer midi;
        const auto sig = voice (48000, 48000.0, -16.0, 3);
        bool fin = true;
        for (size_t pos = 0; pos + 256 <= sig[0].size(); pos += 256)
        {
            for (int c = 0; c < 4; ++c)
                buf.copyFrom (c, 0, sig[(size_t) (c % 2)].data() + pos, 256);
            p.processBlock (buf, midi);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 256; ++i)
                    fin = fin && std::isfinite (buf.getSample (c, i));
        }
        check (fin && p.sidechainConnected(), "processing with the side-chain connected (BLEED on): finite, connection seen");
    }

    //== 8. host bypass keeps the latency alignment (dry, delayed by the reported latency)
    {
        GateProcessor p;
        prepare (p);
        setParam (p, "range", -80.0f);
        const auto sig = voice (48000, 48000.0, -14.0, 4);
        const auto out = render (p, sig, 256, true);
        const int L = p.getLatencySamples();
        double err = 0;
        for (size_t i = (size_t) L + 24000; i < sig[0].size() - 512; ++i)
            err = std::max (err, (double) std::abs (out[0][i] - sig[0][i - (size_t) L]));
        check (err <= 0.0, "host bypass: exactly the dry input, delayed by the reported latency (error " + juce::String (err) + ")");
    }

    //== 9. mono track (1 in / 1 out)
    {
        GateProcessor p;
        auto mono = p.getBusesLayout();
        mono.inputBuses.getReference (0) = juce::AudioChannelSet::mono();
        mono.outputBuses.getReference (0) = juce::AudioChannelSet::mono();
        check (p.setBusesLayout (mono), "mono layout accepted");
        p.setRateAndBufferSizeDetails (44100.0, 300);
        p.prepareToPlay (44100.0, 300);
        const auto sig = voice (44100 * 2, 44100.0, -14.0, 7);
        const auto out = render (p, sig, 300);
        double pk = 0;
        bool fin = true;
        for (float v : out[0])
        {
            fin = fin && std::isfinite (v);
            pk = std::max (pk, (double) std::abs (v));
        }
        check (fin && pk > 0.01 && pk < 1.5, "mono: processes (peak " + juce::String (pk, 3) + ")");
    }

    //== 10. editor: open/close repeatedly while processing, size limits and aspect ratio
    {
        GateProcessor p;
        prepare (p);
        const auto sig = voice (2048, 48000.0, -14.0, 2);
        bool ok = true;
        for (int i = 0; i < 5; ++i)
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            render (p, sig);
            pumpMessages (40);
            ok = ok && ed->getWidth() == gui::kWidth;
        }
        check (ok, "editor: opens/closes 5x while processing (100 % size)");
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        auto* c = ed->getConstrainer();
        check (c != nullptr && c->getMinimumWidth() == gui::kWidth * 3 / 4 && c->getMaximumWidth() == 2 * gui::kWidth
                   && std::abs (c->getFixedAspectRatio() - (double) gui::kWidth / gui::kHeight) < 1e-9,
               "editor: resizable 75-200 %, aspect locked");
        ed->setSize (gui::kWidth * 3 / 2, gui::kHeight * 3 / 2);
        ed.reset();
        std::unique_ptr<juce::AudioProcessorEditor> ed2 (p.createEditor());
        check (ed2->getWidth() == gui::kWidth * 3 / 2, "editor: re-opens at the last used size (150 %)");
    }

    //== 11. 2.0: projects and presets saved by 1.0 load (HOLD keeps its 1.0 tail; new parameters at their defaults)
    {
        // a 1.0 state: the same XML without the 2.0 parameters and without the version tag
        auto v1State = [] (std::initializer_list<std::pair<const char*, float>> values) {
            GateProcessor a;
            for (auto& v : values)
                setParam (a, v.first, v.second);
            setParam (a, "keyHp", 300.0f); // must not survive: 1.0 had no such parameter
            auto tree = a.apvts.copyState();
            for (int i = tree.getNumChildren(); --i >= 0;)
            {
                const auto id = tree.getChild (i).getProperty ("id").toString();
                if (id == "midiNote" || id.startsWith ("level") || id.startsWith ("key"))
                    tree.removeChild (i, nullptr);
            }
            tree.setProperty ("presetName", "Old Project", nullptr);
            juce::MemoryBlock mb;
            juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), mb);
            return mb;
        };
        struct Case { const char* name; std::initializer_list<std::pair<const char*, float>> v; float hold; };
        const Case cases[] = { { "VOCAL SMART BEST, hold 60 -> 25 ms", { { "mode", 0 }, { "detect", 0 }, { "speed", 2 }, { "hold", 60 } }, 25.0f },
                               { "VOCAL SMART FAST, hold 60 -> 35 ms", { { "mode", 0 }, { "detect", 0 }, { "speed", 1 }, { "hold", 60 } }, 35.0f },
                               { "VOCAL SMART BEST, hold 20 -> 0 ms", { { "mode", 0 }, { "detect", 0 }, { "speed", 2 }, { "hold", 20 } }, 0.0f },
                               { "VOCAL LEVEL, hold 80 unchanged", { { "mode", 0 }, { "detect", 1 }, { "speed", 2 }, { "hold", 80 } }, 80.0f },
                               { "VOCAL LIVE, hold 80 unchanged", { { "mode", 0 }, { "detect", 0 }, { "speed", 0 }, { "hold", 80 } }, 80.0f },
                               { "SHAPE, hold 40 unchanged", { { "mode", 1 }, { "hold", 40 } }, 40.0f } };
        for (auto& c : cases)
        {
            const auto mb = v1State (c.v);
            GateProcessor b;
            b.setStateInformation (mb.getData(), (int) mb.getSize());
            const auto r = b.readParams();
            const bool fresh = r.midiNote == 36 && std::abs (r.keyHpHz - 80.0f) < 1e-3f && r.keyLpHz > 19999.0f && ! r.keyListen
                               && r.stepLevel[0] == 1.0f && r.stepLevel[15] == 1.0f;
            check (std::abs (r.holdMs - c.hold) < 1e-3f && fresh && b.getPresetName() == "Old Project",
                   juce::String ("1.0 project: ") + c.name + ", 2.0 parameters at defaults, name kept (hold " + juce::String (r.holdMs) + ")");
        }
        // a 2.0 state is not converted again
        {
            GateProcessor a;
            setParam (a, "hold", 60.0f);
            juce::MemoryBlock mb;
            a.getStateInformation (mb);
            GateProcessor b;
            b.setStateInformation (mb.getData(), (int) mb.getSize());
            check (std::abs (getParam (b, "hold") - 60.0f) < 1e-3f, "2.0 project: HOLD loads as saved (60 ms)");
        }
        // a 1.0 user preset file
        {
            const auto mb = v1State ({ { "mode", 0 }, { "detect", 0 }, { "speed", 2 }, { "hold", 100 } });
            auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
            auto f = GateProcessor::userPresetFolder().getChildFile ("Old Preset.axgpreset");
            GateProcessor::userPresetFolder().createDirectory();
            xml->writeTo (f);
            GateProcessor q;
            check (q.loadUserPreset (f) && std::abs (getParam (q, "hold") - 65.0f) < 1e-3f, "1.0 user preset: loads, HOLD 100 -> 65 ms");
            f.deleteFile();
        }
    }

    //== 12. 2.0: what BLEED learned is saved with the project and restored
    {
        auto makeSc = [] (GateProcessor& p) {
            auto layout = p.getBusesLayout();
            layout.inputBuses.getReference (1) = juce::AudioChannelSet::stereo();
            p.setBusesLayout (layout);
            setParam (p, "bleed", 1.0f); // before prepare: a configuration (latency) change waits for the host's handshake
            setParam (p, "range", 0.0f);
            p.setRateAndBufferSizeDetails (48000.0, 256);
            p.prepareToPlay (48000.0, 256);
        };
        // the beat and its leak (-30 dB, 1 ms) into a voice
        const int n = 48000 * 8;
        const auto v = voice (n, 48000.0, -18.0, 12);
        std::vector<float> beat ((size_t) n);
        std::mt19937 g (5);
        std::normal_distribution<float> nd;
        float lp = 0;
        for (auto& b : beat)
        {
            lp = 0.7f * lp + 0.3f * nd (g);
            b = 0.2f * lp;
        }
        auto run = [&] (GateProcessor& p, int len, double& leakIn, double& leakOut) {
            juce::AudioBuffer<float> buf (4, 256);
            juce::MidiBuffer midi;
            const int L = p.getLatencySamples();
            std::vector<float> out ((size_t) len + 256);
            for (int pos = 0; pos + 256 <= len; pos += 256)
            {
                for (int i = 0; i < 256; ++i)
                {
                    const int t = pos + i;
                    const float lk = t >= 48 ? 0.0316f * beat[(size_t) (t - 48)] : 0.0f;
                    buf.setSample (0, i, v[0][(size_t) t] + lk);
                    buf.setSample (1, i, v[0][(size_t) t] + lk);
                    buf.setSample (2, i, beat[(size_t) t]);
                    buf.setSample (3, i, beat[(size_t) t]);
                }
                p.processBlock (buf, midi);
                std::copy (buf.getReadPointer (0), buf.getReadPointer (0) + 256, out.begin() + pos);
            }
            leakIn = leakOut = 0;
            for (int t = 2400; t + L < len && t < 48000; ++t) // the first second (after 50 ms)
            {
                const float lk = 0.0316f * beat[(size_t) (t - 48)];
                const double res = (double) out[(size_t) (t + L)] - v[0][(size_t) t];
                leakIn += (double) lk * lk;
                leakOut += res * res;
            }
        };
        GateProcessor a;
        makeSc (a);
        double i0, o0;
        run (a, n, i0, o0);
        pumpMessages (100);
        juce::MemoryBlock state;
        a.getStateInformation (state);
        auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        check (xml != nullptr && xml->hasAttribute ("bleedPath") && (int) state.getSize() < 200000,
               "BLEED: the learned path is in the project state (" + juce::String ((int) state.getSize() / 1024) + " KB)");
        GateProcessor fresh, restored;
        makeSc (fresh);
        restored.setStateInformation (state.getData(), (int) state.getSize()); // before prepare, as a host loading a project
        makeSc (restored);
        double fi, fo, ri, ro;
        run (fresh, 48000 + 8192, fi, fo);
        run (restored, 48000 + 8192, ri, ro);
        const double gf = 10.0 * std::log10 (fi / fo), gr = 10.0 * std::log10 (ri / ro);
        check (gr > gf + 6.0 && gr > 10.0, "BLEED restored from the project: first second " + juce::String (gr, 1) + " dB of bleed removed (fresh: "
                                                + juce::String (gf, 1) + " dB)");
        juce::MemoryBlock again;
        restored.getStateInformation (again);
        auto xml2 = juce::AudioProcessor::getXmlFromBinary (again.getData(), (int) again.getSize());
        check (xml2 != nullptr && xml2->hasAttribute ("bleedPath"), "BLEED: re-saved project keeps the path");
        GateProcessor plain;
        juce::MemoryBlock ps;
        plain.getStateInformation (ps);
        auto xml3 = juce::AudioProcessor::getXmlFromBinary (ps.getData(), (int) ps.getSize());
        check (xml3 != nullptr && ! xml3->hasAttribute ("bleedPath"), "BLEED never used: no path in the state");
    }

    //== 13. 2.0: SHAPE sends a MIDI note per hit through the host's MIDI buffer
    {
        GateProcessor p;
        p.setCurrentProgram (21); // Kick Trigger (MIDI); before prepare (latency handshake)
        prepare (p);
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer midi;
        int ons = 0, offs = 0, wrongNote = 0;
        const int n = 48000 * 3;
        for (int pos = 0; pos + 256 <= n; pos += 256)
        {
            for (int i = 0; i < 256; ++i)
            {
                const double t = std::fmod ((pos + i) / 48000.0, 0.5); // a kick every 0.5 s
                const float y = (float) (0.7 * std::sin (2 * juce::MathConstants<double>::pi * (50 + 100 * std::exp (-t * 30)) * t) * std::exp (-t * 8));
                buf.setSample (0, i, y);
                buf.setSample (1, i, y);
            }
            midi.clear();
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0); // incoming MIDI is not passed on
            p.processBlock (buf, midi);
            for (const auto m : midi)
            {
                const auto msg = m.getMessage();
                ons += msg.isNoteOn() ? 1 : 0;
                offs += msg.isNoteOff() ? 1 : 0;
                wrongNote += (msg.isNoteOnOrOff() && msg.getNoteNumber() != 36) ? 1 : 0;
            }
        }
        check (ons == 6 && offs == 6 && wrongNote == 0, "SHAPE MIDI out: a note C1 per kick (" + juce::String (ons) + " on / "
                                                             + juce::String (offs) + " off), incoming MIDI not passed on");
    }

    tmp.deleteRecursively();
    std::printf ("%s: %d of %d plugin checks passed\n", failures == 0 ? "ALL PASS" : "FAILURES", checks - failures, checks);
    return failures;
}
