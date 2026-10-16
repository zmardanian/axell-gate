// Axell Gate - headless GUI snapshot tool: builds the real processor + editor, feeds a vocal (a WAV given as the second argument, or
// a synthetic voice with sibilants) so the displays move, and saves PNGs.   GateGuiSnapshot [outputDir] [vocal.wav]
#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <functional>
#include <random>

namespace
{
void setParam (GateProcessor& p, const char* id, float value)
{
    if (auto* prm = p.apvts.getParameter (id))
        prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

// synthetic voice: 140 Hz pulse train through vowel formants, with 120 ms hiss bursts (sibilants) every 0.7 s
juce::AudioBuffer<float> syntheticVoice (double fs, double seconds)
{
    const int n = (int) (seconds * fs);
    juce::AudioBuffer<float> b (2, n);
    std::mt19937 g (3);
    std::normal_distribution<double> nd;
    const double pi = juce::MathConstants<double>::pi;
    double ph = 0.0, s1 = 0, s2 = 0, h1 = 0, h2 = 0;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs, cyc = std::fmod (t, 0.7);
        const bool ess = cyc > 0.5 && cyc < 0.62;
        ph += 2 * pi * (140.0 + 10.0 * std::sin (2 * pi * 0.5 * t)) / fs;
        double v = 0;
        for (int k = 1; k < 40; ++k)
        {
            const double f = 140.0 * k, a = std::exp (-std::pow ((f - 700) / 300, 2)) + 0.5 * std::exp (-std::pow ((f - 1200) / 400, 2))
                                            + 0.15 * std::exp (-std::pow ((f - 2800) / 600, 2));
            v += a * std::sin (k * ph) / k;
        }
        // hiss: two-pole resonance at 7 kHz
        const double w = 2 * pi * 7000 / fs, r = 0.93;
        const double x = nd (g);
        const double hs = x + 2 * r * std::cos (w) * h1 - r * r * h2;
        h2 = h1;
        h1 = hs;
        (void) s1; (void) s2;
        const double env = ess ? std::sin (pi * (cyc - 0.5) / 0.12) : 0.0;
        const double y = 0.12 * (ess ? 0.15 : 1.0) * v + 0.02 * env * hs;
        b.setSample (0, i, (float) y);
        b.setSample (1, i, (float) y);
    }
    return b;
}

void shoot (const juce::File& dir, const juce::String& name, float uiScale, float renderScale, const juce::AudioBuffer<float>& voice,
            const std::function<void (GateProcessor&)>& setup, bool beat = false)
{
    GateProcessor proc;
    if (beat) // 2.0: the beat on the side-chain (BLEED meter)
    {
        juce::AudioProcessor::BusesLayout l;
        l.inputBuses.add (juce::AudioChannelSet::stereo());
        l.inputBuses.add (juce::AudioChannelSet::stereo());
        l.outputBuses.add (juce::AudioChannelSet::stereo());
        proc.setBusesLayout (l);
        proc.setRateAndBufferSizeDetails (48000.0, 512);
    }
    else
        proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);
    setup (proc);
    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
    auto* editor = dynamic_cast<GateEditor*> (ed.get());
    ed->setSize (juce::roundToInt (gui::kWidth * uiScale), juce::roundToInt (gui::kHeight * uiScale));
    juce::AudioBuffer<float> buf (beat ? 4 : 2, 512);
    juce::MidiBuffer midi;
    const int n = voice.getNumSamples();
    std::vector<float> leak (4096, 0.0f);
    std::mt19937 g (11);
    std::normal_distribution<float> nd;
    float lp = 0.0f;
    for (int pos = 0, blk = 0; pos + 512 <= n; pos += 512, ++blk)
    {
        for (int c = 0; c < 2; ++c)
            buf.copyFrom (c, 0, voice, std::min (c, voice.getNumChannels() - 1), pos, 512);
        if (beat)
            for (int i = 0; i < 512; ++i)
            {
                // a busy beat on 3/4, and its headphone leak (-30 dB, 1 ms late) in the mic
                lp = 0.7f * lp + 0.3f * nd (g);
                const float b = 0.25f * lp * (0.6f + 0.4f * std::sin (6.2831853f * 2.0f * (float) (pos + i) / 48000.0f));
                buf.setSample (2, i, b);
                buf.setSample (3, i, b);
                leak[(size_t) ((pos + i + 48) % leak.size())] = 0.0316f * b;
                for (int c = 0; c < 2; ++c)
                    buf.addSample (c, i, leak[(size_t) ((pos + i) % leak.size())]);
            }
        proc.processBlock (buf, midi);
        if (blk % 3 == 0)
            editor->refreshNow();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
    }
    editor->refreshNow();
    const auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, renderScale);
    const auto file = dir.getChildFile (name + ".png");
    file.deleteFile();
    juce::FileOutputStream os (file);
    juce::PNGImageFormat png;
    if (os.openedOk() && png.writeImageToStream (img, os))
        std::printf ("wrote %s (%dx%d)\n", file.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());
    else
        std::printf ("FAILED to write %s\n", file.getFullPathName().toRawUTF8());
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File dir (argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[1])
                                   : juce::File::getCurrentWorkingDirectory().getChildFile ("docs/screenshots"));
    dir.createDirectory();
    juce::AudioBuffer<float> voice;
    float level = 0.0f;
    if (argc > 2)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (juce::File (argv[2])));
        if (r != nullptr)
        {
            const int len = (int) std::min<juce::int64> (r->lengthInSamples, (juce::int64) (12.0 * r->sampleRate));
            const auto start = std::min<juce::int64> (r->lengthInSamples - len, (juce::int64) (8.0 * r->sampleRate));
            voice.setSize ((int) r->numChannels, len);
            r->read (&voice, 0, len, start, true, true);
            level = 15.0f;
        }
    }
    if (voice.getNumSamples() == 0)
        voice = syntheticVoice (48000.0, 8.0);
    auto vocal = [level] (GateProcessor& p) { juce::ignoreUnused (level); p.setCurrentProgram (1); };
    shoot (dir, "gate-vocal", 1.0f, 1.0f, voice, vocal);
    shoot (dir, "gate-vocal-hidpi2x", 1.0f, 2.0f, voice, vocal);
    shoot (dir, "gate-75pct", 0.75f, 1.0f, voice, vocal);
    shoot (dir, "gate-shape", 1.0f, 1.0f, voice, [] (GateProcessor& p) { p.setCurrentProgram (8); });
    shoot (dir, "gate-rhythm", 1.0f, 1.0f, voice, [] (GateProcessor& p) { p.setCurrentProgram (15); });
    // 2.0 screens
    shoot (dir, "gate-bleed", 1.0f, 1.0f, voice, [] (GateProcessor& p) { p.setCurrentProgram (2); }, true);
    shoot (dir, "gate-level-key", 1.0f, 1.0f, voice, [] (GateProcessor& p) { p.setCurrentProgram (24); });
    shoot (dir, "gate-rhythm-levels", 1.0f, 1.0f, voice, [] (GateProcessor& p) { p.setCurrentProgram (22); });
    shoot (dir, "gate-shape-midi", 1.0f, 1.0f, voice, [] (GateProcessor& p) { p.setCurrentProgram (21); });
    return 0;
}
