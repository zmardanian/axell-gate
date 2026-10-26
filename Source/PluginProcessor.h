#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Engine.h"
#include "Presets.h"

// Thin JUCE wrapper around axg::Engine (all DSP lives in JUCE-free headers).
class GateProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    GateProcessor();
    ~GateProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;
    void setNonRealtime (bool isNonRealtime) noexcept override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return true; } // 2.0: SHAPE sends a note per hit
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram.load(); }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::AudioProcessorValueTreeState apvts;

    void applyParams (const axg::Params& p);
    axg::Params readParams() const;
    static juce::File userPresetFolder();
    bool saveUserPreset (const juce::String& name);
    bool loadUserPreset (const juce::File& file);
    juce::String getPresetName() const;
    void setPresetName (const juce::String& n);
    int getActiveSlot() const { return activeSlot; }
    void selectSlot (int slot);
    void copyActiveToOther();
    bool sidechainConnected() const { return scConnected.load(); }

    axg::Engine& engine() { return dsp; }

private:
    void timerCallback() override;
    void syncEngineParams (bool bypass);
    void restoreTree (const juce::ValueTree& tree);
    void run (juce::AudioBuffer<float>& buffer, bool bypass, juce::MidiBuffer& midi);

    axg::Engine dsp;
    int preparedChannels = 0;
    std::vector<float> loadedPath; // BLEED path from the project, until the engine has it (message thread)
    double loadedPathFs = 0.0;
    std::atomic<int> currentProgram { 0 };
    std::atomic<bool> programNamePending { false }, scConnected { false };
    juce::CriticalSection nameLock;
    juce::String presetName { "Init" };
    int activeSlot = 0;
    juce::ValueTree slots[2];
    double ppqFree = 0.0;

    std::atomic<float>* pMode = nullptr;  std::atomic<float>* pDetect = nullptr;  std::atomic<float>* pSpeed = nullptr;
    std::atomic<float>* pThr = nullptr;   std::atomic<float>* pSens = nullptr;    std::atomic<float>* pBreath = nullptr;
    std::atomic<float>* pBleed = nullptr; std::atomic<float>* pFreeze = nullptr;  std::atomic<float>* pRange = nullptr;
    std::atomic<float>* pAttack = nullptr; std::atomic<float>* pHold = nullptr;   std::atomic<float>* pRelease = nullptr;
    std::atomic<float>* pTail = nullptr;  std::atomic<float>* pSensDb = nullptr;  std::atomic<float>* pRate = nullptr;
    std::atomic<float>* pSteps = nullptr; std::atomic<float>* pSwing = nullptr;   std::atomic<float>* pEdge = nullptr;
    std::atomic<float>* pOutput = nullptr;
    std::array<std::atomic<float>*, 16> pStep {}, pLevel {};
    std::atomic<float>* pMidiNote = nullptr; std::atomic<float>* pKeyHp = nullptr; std::atomic<float>* pKeyLp = nullptr;
    std::atomic<float>* pKeyListen = nullptr;
    std::atomic<float>* pPitchHold = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GateProcessor)
};
