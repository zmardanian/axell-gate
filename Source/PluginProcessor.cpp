#include "PluginProcessor.h"
#include "PluginEditor.h"

using APVTS = juce::AudioProcessorValueTreeState;

namespace
{
juce::AudioParameterFloatAttributes fmt (std::function<juce::String (float)> toText,
                                         std::function<float (const juce::String&)> fromText = nullptr)
{
    auto a = juce::AudioParameterFloatAttributes().withStringFromValueFunction ([toText] (float v, int) { return toText (v); });
    if (fromText == nullptr)
        fromText = [] (const juce::String& t) { return t.getFloatValue(); };
    return a.withValueFromStringFunction (fromText);
}
juce::String dbText (float v) { return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " dB"; }
juce::String msText (float v) { return v < 10.0f ? juce::String (v, 2) + " ms" : juce::String (juce::roundToInt (v)) + " ms"; }
juce::String pctText (float v) { return juce::String (juce::roundToInt (v)) + " %"; }
const juce::Identifier kPresetNameId ("presetName");
const juce::Identifier kStateVersionId ("stateVersion"), kBleedPathId ("bleedPath"), kBleedFsId ("bleedFs");
constexpr int kStateVersion = 3;   // 3 = 4.0 (PITCH HOLD)

// 1.0 states and presets: VOCAL SMART closed as early as it opened (35 ms BEST, 25 ms FAST), so the tail after the
// last voice frame was HOLD minus that. 2.0 closes at the decision: keep each saved tail
void upgradeTree (juce::ValueTree& tree)
{
    const int version = (int) tree.getProperty (kStateVersionId, 1);
    // 4.0: PITCH HOLD is on for new instances; a state saved before it existed plays as it did - off
    if (version < 3 && ! tree.getChildWithProperty ("id", "pitchHold").isValid())
    {
        juce::ValueTree c ("PARAM");
        c.setProperty ("id", "pitchHold", nullptr);
        c.setProperty ("value", 0.0f, nullptr);
        tree.appendChild (c, nullptr);
    }
    if (version >= 2)
    {
        tree.setProperty (kStateVersionId, kStateVersion, nullptr);
        return;
    }
    auto value = [&] (const char* id, float def) {
        const auto c = tree.getChildWithProperty ("id", id);
        return c.isValid() && c.hasProperty ("value") ? (float) c.getProperty ("value") : def;
    };
    const int mode = juce::roundToInt (value ("mode", 0)), detect = juce::roundToInt (value ("detect", 0));
    const int speed = juce::roundToInt (value ("speed", 2));
    auto hold = tree.getChildWithProperty ("id", "hold");
    if (mode == axg::kVocal && detect == axg::kSmart && speed != axg::kLive && hold.isValid() && hold.hasProperty ("value"))
        hold.setProperty ("value", juce::jmax (0.0f, (float) hold.getProperty ("value") - (speed == axg::kBest ? 35.0f : 25.0f)), nullptr);
    tree.setProperty (kStateVersionId, kStateVersion, nullptr);
}
} // namespace

APVTS::ParameterLayout GateProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    // IDs, ranges and order are frozen from 1.0 on (saved projects, automation); new parameters are appended
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Mode", StringArray { "Vocal", "Shape", "Rhythm" }, 0));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "detect", 1 }, "Detection", StringArray { "Smart", "Level" }, 0));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "speed", 1 }, "Speed", StringArray { "Live", "Fast", "Best" }, 2));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "threshold", 1 }, "Threshold",
        NormalisableRange<float> (-90.0f, 0.0f, 0.1f), -45.0f, fmt ([] (float v) { return String (v, 1) + " dB"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "sensitivity", 1 }, "Sensitivity",
        NormalisableRange<float> (-100.0f, 100.0f, 1.0f), 0.0f, fmt ([] (float v) { return (v > 0.5f ? "+" : "") + String (juce::roundToInt (v)); })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "breath", 1 }, "Breath Level",
        NormalisableRange<float> (-80.0f, 0.0f, 0.1f), -12.0f, fmt ([] (float v) { return v <= -79.9f ? String ("Removed") : dbText (v); })));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "bleed", 1 }, "Bleed Removal", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "bleedFreeze", 1 }, "Bleed Freeze", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "range", 1 }, "Range",
        NormalisableRange<float> (-80.0f, 0.0f, 0.1f), -60.0f, fmt (dbText)));
    NormalisableRange<float> att (0.05f, 50.0f, 0.01f);
    att.setSkewForCentre (2.0f);
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "attack", 1 }, "Attack", att, 2.0f, fmt (msText)));
    NormalisableRange<float> hold (0.0f, 2000.0f, 0.1f);
    hold.setSkewForCentre (80.0f);
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "hold", 1 }, "Hold", hold, 30.0f, fmt (msText)));
    NormalisableRange<float> rel (1.0f, 2000.0f, 0.1f);
    rel.setSkewForCentre (80.0f);
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "release", 1 }, "Release", rel, 50.0f, fmt (msText)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "tail", 1 }, "Tail",
        NormalisableRange<float> (5.0f, 100.0f, 0.1f), 100.0f, fmt ([] (float v) { return v >= 99.95f ? String ("Natural") : pctText (v); })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "sensDb", 1 }, "Hit Sensitivity",
        NormalisableRange<float> (1.0f, 30.0f, 0.1f), 9.0f, fmt ([] (float v) { return String (v, 1) + " dB"; })));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "rate", 1 }, "Rate",
        StringArray { "1/4", "1/8", "1/16", "1/32", "1/8T", "1/16T" }, 2));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "steps", 1 }, "Pattern Length", 2, 16, 16));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "swing", 1 }, "Swing",
        NormalisableRange<float> (0.0f, 100.0f, 1.0f), 0.0f, fmt (pctText)));
    NormalisableRange<float> edge (0.1f, 30.0f, 0.01f);
    edge.setSkewForCentre (3.0f);
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "edge", 1 }, "Edge", edge, 3.0f, fmt (msText)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "output", 1 }, "Output",
        NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, fmt (dbText)));
    for (int k = 0; k < 16; ++k)
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "step" + String (k + 1), 1 }, "Step " + String (k + 1), k % 2 == 0));
    // 2.0 (appended)
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "midiNote", 2 }, "Hit Note", 0, 127, 36,
        AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return MidiMessage::getMidiNoteName (v, true, true, 3); })));
    for (int k = 0; k < 16; ++k)
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "level" + String (k + 1), 2 }, "Step " + String (k + 1) + " Level",
            NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, fmt (pctText)));
    NormalisableRange<float> khp (20.0f, 2000.0f, 1.0f);
    khp.setSkewForCentre (150.0f);
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "keyHp", 2 }, "Key High-Pass", khp, 80.0f,
        fmt ([] (float v) { return String (juce::roundToInt (v)) + " Hz"; })));
    NormalisableRange<float> klp (1000.0f, 20000.0f, 10.0f);
    klp.setSkewForCentre (5000.0f);
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "keyLp", 2 }, "Key Low-Pass", klp, 20000.0f,
        fmt ([] (float v) { return v >= 19990.0f ? String ("Off") : String (v / 1000.0f, 1) + " kHz"; })));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "keyListen", 2 }, "Key Listen", false));
    // 4.0: LEVEL detection - a pitched voice under the threshold keeps the gate open (soft syllables, voiced tails)
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "pitchHold", 3 }, "Pitch Hold", true));
    return { p.begin(), p.end() };
}

GateProcessor::GateProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Beat (side-chain)", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    pMode = apvts.getRawParameterValue ("mode");
    pDetect = apvts.getRawParameterValue ("detect");
    pSpeed = apvts.getRawParameterValue ("speed");
    pThr = apvts.getRawParameterValue ("threshold");
    pSens = apvts.getRawParameterValue ("sensitivity");
    pBreath = apvts.getRawParameterValue ("breath");
    pBleed = apvts.getRawParameterValue ("bleed");
    pFreeze = apvts.getRawParameterValue ("bleedFreeze");
    pRange = apvts.getRawParameterValue ("range");
    pAttack = apvts.getRawParameterValue ("attack");
    pHold = apvts.getRawParameterValue ("hold");
    pRelease = apvts.getRawParameterValue ("release");
    pTail = apvts.getRawParameterValue ("tail");
    pSensDb = apvts.getRawParameterValue ("sensDb");
    pRate = apvts.getRawParameterValue ("rate");
    pSteps = apvts.getRawParameterValue ("steps");
    pSwing = apvts.getRawParameterValue ("swing");
    pEdge = apvts.getRawParameterValue ("edge");
    pOutput = apvts.getRawParameterValue ("output");
    for (int k = 0; k < 16; ++k)
    {
        pStep[(size_t) k] = apvts.getRawParameterValue ("step" + juce::String (k + 1));
        pLevel[(size_t) k] = apvts.getRawParameterValue ("level" + juce::String (k + 1));
    }
    pMidiNote = apvts.getRawParameterValue ("midiNote");
    pKeyHp = apvts.getRawParameterValue ("keyHp");
    pKeyLp = apvts.getRawParameterValue ("keyLp");
    pKeyListen = apvts.getRawParameterValue ("keyListen");
    pPitchHold = apvts.getRawParameterValue ("pitchHold");
    startTimerHz (20);
}

GateProcessor::~GateProcessor() { stopTimer(); }

bool GateProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (! (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo()) || out != layouts.getMainInputChannelSet())
        return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.inputBuses[1];
        return sc.isDisabled() || sc == juce::AudioChannelSet::mono() || sc == juce::AudioChannelSet::stereo();
    }
    return true;
}

axg::Params GateProcessor::readParams() const
{
    axg::Params p;
    p.mode = juce::roundToInt (pMode->load());
    p.detect = juce::roundToInt (pDetect->load());
    p.speed = juce::roundToInt (pSpeed->load());
    p.thresholdDb = pThr->load();
    p.sensitivity = pSens->load();
    p.breathDb = pBreath->load();
    p.bleed = pBleed->load() > 0.5f;
    p.bleedFreeze = pFreeze->load() > 0.5f;
    p.rangeDb = pRange->load();
    p.attackMs = pAttack->load();
    p.holdMs = pHold->load();
    p.releaseMs = pRelease->load();
    p.tailPct = pTail->load();
    p.sensDb = pSensDb->load();
    p.rate = juce::roundToInt (pRate->load());
    p.steps = juce::roundToInt (pSteps->load());
    p.swingPct = pSwing->load();
    p.edgeMs = pEdge->load();
    p.outputDb = pOutput->load();
    uint32_t pat = 0;
    for (int k = 0; k < 16; ++k)
        if (pStep[(size_t) k]->load() > 0.5f)
            pat |= 1u << k;
    p.pattern = pat;
    for (int k = 0; k < 16; ++k)
        p.stepLevel[(size_t) k] = pLevel[(size_t) k]->load() * 0.01f;
    p.midiNote = juce::roundToInt (pMidiNote->load());
    p.keyHpHz = pKeyHp->load();
    p.keyLpHz = pKeyLp->load();
    p.keyListen = pKeyListen->load() > 0.5f;
    p.pitchHold = pPitchHold->load() > 0.5f;
    return p;
}

void GateProcessor::syncEngineParams (bool bypass)
{
    auto p = readParams();
    p.bypass = bypass;
    dsp.setParams (p);
}

void GateProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    preparedChannels = juce::jlimit (1, axell::kMaxCh, getMainBusNumOutputChannels());
    syncEngineParams (false);
    dsp.prepare (sampleRate, preparedChannels, samplesPerBlock);
    if (! loadedPath.empty())
        dsp.setBleedPath (loadedPath.data(), (int) loadedPath.size(), loadedPathFs);
    dsp.handshake().setNonRealtime (isNonRealtime());
    setLatencySamples (dsp.getLatencySamples());
    dsp.handshake().setHostLatency (getLatencySamples());
}

void GateProcessor::reset()
{
    if (preparedChannels > 0)
        dsp.reset();
}

void GateProcessor::setNonRealtime (bool isOffline) noexcept
{
    AudioProcessor::setNonRealtime (isOffline);
    dsp.handshake().setNonRealtime (isOffline);
}

void GateProcessor::run (juce::AudioBuffer<float>& buffer, bool bypass, juce::MidiBuffer& midi)
{
    midi.clear(); // SHAPE sends a note per hit; incoming MIDI is not used
    juce::ScopedNoDenormals noDenormals;
    const int mainIn = getMainBusNumInputChannels();
    for (int c = mainIn; c < getMainBusNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());
    if (preparedChannels == 0 || buffer.getNumSamples() == 0)
        return;
    syncEngineParams (bypass);
    // host transport for RHYTHM (tempo, position, playing); without one the pattern free-runs at the last tempo
    axg::Transport t;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto bpm = pos->getBpm())
                t.bpm = *bpm;
            if (auto ppq = pos->getPpqPosition())
            {
                t.ppq = *ppq;
                t.valid = true;
            }
            t.playing = pos->getIsPlaying();
        }
    dsp.setTransport (t);
    // side-chain bus (the beat for BLEED)
    const float* sc[2] {};
    int scCh = 0;
    if (getBusCount (true) > 1 && getBus (true, 1)->isEnabled())
    {
        auto scBuf = getBusBuffer (buffer, true, 1);
        scCh = juce::jmin (2, scBuf.getNumChannels());
        for (int c = 0; c < scCh; ++c)
            sc[c] = scBuf.getReadPointer (c);
    }
    scConnected.store (scCh > 0);
    auto mainBuf = getBusBuffer (buffer, false, 0);
    dsp.process (mainBuf.getArrayOfWritePointers(), juce::jmin (mainBuf.getNumChannels(), preparedChannels), buffer.getNumSamples(), sc, scCh);
    // the hits' notes, at the hits as heard in the (latency-compensated) output; bypassed: note offs only
    const axg::MidiEvent* ev = nullptr;
    const int ne = dsp.midiEvents (ev);
    for (int k = 0; k < ne; ++k)
    {
        const int at = juce::jlimit (0, buffer.getNumSamples() - 1, ev[k].offset);
        if (ev[k].velocity > 0 && ! bypass)
            midi.addEvent (juce::MidiMessage::noteOn (1, ev[k].note, (juce::uint8) ev[k].velocity), at);
        else if (ev[k].velocity == 0)
            midi.addEvent (juce::MidiMessage::noteOff (1, ev[k].note), at);
    }
}

void GateProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) { run (buffer, false, midi); }

// host bypass: the engine keeps running (latency-aligned dry, 10 ms crossfade instead of a click)
void GateProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) { run (buffer, true, midi); }

void GateProcessor::timerCallback()
{
    if (programNamePending.exchange (false))
        setPresetName (getProgramName (currentProgram.load()));
    if (! loadedPath.empty() && dsp.bleedLearned())
        loadedPath.clear(); // the engine has it (and saves its own, newer copy)
    auto& hs = dsp.handshake();
    hs.heartbeat();
    int lat = 0;
    if (preparedChannels > 0 && hs.take (lat))
    {
        if (lat != getLatencySamples())
            setLatencySamples (lat);
        hs.ack();
    }
}

//== presets, A/B ==============================================================================================
void GateProcessor::applyParams (const axg::Params& p)
{
    auto set = [this] (const juce::String& id, float v)
    {
        if (auto* prm = apvts.getParameter (id))
            prm->setValueNotifyingHost (prm->convertTo0to1 (v));
    };
    set ("mode", (float) p.mode);
    set ("detect", (float) p.detect);
    set ("speed", (float) p.speed);
    set ("threshold", p.thresholdDb);
    set ("sensitivity", p.sensitivity);
    set ("breath", p.breathDb);
    set ("bleed", p.bleed ? 1.0f : 0.0f);
    set ("bleedFreeze", 0.0f);
    set ("range", p.rangeDb);
    set ("attack", p.attackMs);
    set ("hold", p.holdMs);
    set ("release", p.releaseMs);
    set ("tail", p.tailPct);
    set ("sensDb", p.sensDb);
    set ("rate", (float) p.rate);
    set ("steps", (float) p.steps);
    set ("swing", p.swingPct);
    set ("edge", p.edgeMs);
    set ("output", p.outputDb);
    for (int k = 0; k < 16; ++k)
        set ("step" + juce::String (k + 1), ((p.pattern >> k) & 1u) ? 1.0f : 0.0f);
    for (int k = 0; k < 16; ++k)
        set ("level" + juce::String (k + 1), p.stepLevel[(size_t) k] * 100.0f);
    set ("midiNote", (float) p.midiNote);
    set ("keyHp", p.keyHpHz);
    set ("keyLp", p.keyLpHz);
    set ("keyListen", 0.0f);
    set ("pitchHold", p.pitchHold ? 1.0f : 0.0f);
}

int GateProcessor::getNumPrograms() { return (int) axg::factoryPresets().size(); }

void GateProcessor::setCurrentProgram (int index)
{
    const auto list = axg::factoryPresets();
    if (index < 0 || index >= (int) list.size())
        return;
    currentProgram.store (index);
    applyParams (list[(size_t) index].params);
    if (juce::MessageManager::existsAndIsCurrentThread())
        setPresetName (list[(size_t) index].name);
    else
        programNamePending.store (true);
}

const juce::String GateProcessor::getProgramName (int index)
{
    const auto list = axg::factoryPresets();
    return index >= 0 && index < (int) list.size() ? juce::String (list[(size_t) index].name) : juce::String();
}

juce::String GateProcessor::getPresetName() const
{
    const juce::ScopedLock sl (nameLock);
    return presetName;
}

void GateProcessor::setPresetName (const juce::String& n)
{
    const juce::ScopedLock sl (nameLock);
    presetName = n;
}

juce::File GateProcessor::userPresetFolder()
{
    auto base = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_HOME", {});
    juce::File root = base.isNotEmpty() ? juce::File (base)
                                        : juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".local/share");
    return root.getChildFile ("AxellGate").getChildFile ("Presets");
}

bool GateProcessor::saveUserPreset (const juce::String& name)
{
    const auto clean = juce::File::createLegalFileName (name.trim());
    auto folder = userPresetFolder();
    if (clean.isEmpty() || ! folder.createDirectory())
        return false;
    auto state = apvts.copyState();
    state.setProperty (kPresetNameId, name.trim(), nullptr);
    state.setProperty (kStateVersionId, kStateVersion, nullptr);
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    if (xml == nullptr || ! xml->writeTo (folder.getChildFile (clean + ".axgpreset")))
        return false;
    setPresetName (name.trim());
    return true;
}

bool GateProcessor::loadUserPreset (const juce::File& file)
{
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return false;
    auto tree = juce::ValueTree::fromXml (*xml);
    upgradeTree (tree);
    restoreTree (tree);
    setPresetName (tree.getProperty (kPresetNameId, file.getFileNameWithoutExtension()).toString());
    return true;
}

void GateProcessor::selectSlot (int slot)
{
    slot = juce::jlimit (0, 1, slot);
    if (slot == activeSlot)
        return;
    slots[activeSlot] = apvts.copyState();
    if (slots[slot].isValid())
        restoreTree (slots[slot].createCopy());
    else
        slots[slot] = apvts.copyState();
    activeSlot = slot;
}

void GateProcessor::copyActiveToOther() { slots[1 - activeSlot] = apvts.copyState(); }

// APVTS::replaceState skips a parameter whose snapped value already matches; re-apply explicitly (pluginval scenario)
void GateProcessor::restoreTree (const juce::ValueTree& tree)
{
    apvts.replaceState (tree);
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            const auto child = apvts.state.getChildWithProperty ("id", rp->paramID);
            if (child.isValid() && child.hasProperty ("value"))
            {
                const float norm = rp->convertTo0to1 ((float) child.getProperty ("value"));
                if (std::abs (rp->getValue() - norm) > 1.0e-6f)
                    rp->setValueNotifyingHost (norm);
            }
        }
}

// the project state: the parameters, the preset name and (2.0) what BLEED has learned, so the project reopens
// already cancelling (same sample rate; otherwise it learns again)
void GateProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty (kPresetNameId, getPresetName(), nullptr);
    state.setProperty (kStateVersionId, kStateVersion, nullptr);
    std::vector<float> path;
    double rate = 0.0;
    if (dsp.copyBleedPath (path, rate) || (! loadedPath.empty() && (path = loadedPath, rate = loadedPathFs, true)))
    {
        juce::MemoryBlock mb (path.data(), path.size() * sizeof (float));
        state.setProperty (kBleedPathId, mb.toBase64Encoding(), nullptr);
        state.setProperty (kBleedFsId, rate, nullptr);
    }
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void GateProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
    {
        auto tree = juce::ValueTree::fromXml (*xml);
        upgradeTree (tree);
        setPresetName (tree.getProperty (kPresetNameId, getPresetName()).toString());
        juce::MemoryBlock mb;
        if (tree.hasProperty (kBleedPathId) && mb.fromBase64Encoding (tree.getProperty (kBleedPathId).toString())
            && mb.getSize() % sizeof (float) == 0 && mb.getSize() > 0)
        {
            loadedPath.assign ((const float*) mb.getData(), (const float*) mb.getData() + mb.getSize() / sizeof (float));
            loadedPathFs = (double) tree.getProperty (kBleedFsId, 0.0);
            dsp.setBleedPath (loadedPath.data(), (int) loadedPath.size(), loadedPathFs);
        }
        tree.removeProperty (kBleedPathId, nullptr);
        tree.removeProperty (kBleedFsId, nullptr);
        restoreTree (tree);
    }
}

juce::AudioProcessorEditor* GateProcessor::createEditor() { return new GateEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new GateProcessor(); }
