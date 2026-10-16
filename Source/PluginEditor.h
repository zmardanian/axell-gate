#pragma once
#include <axell/gui/LookAndFeel.h>
#include <array>
#include "PluginProcessor.h"

namespace gui
{
constexpr int kWidth = 1120;
constexpr int kHeight = 700;
}

// scrolling history (~4 s): input and output level, the gate's gain (from the top) and, in VOCAL, the voice detector's
// confidence as a strip along the bottom
class GateHistory : public juce::Component, public juce::SettableTooltipClient
{
public:
    GateHistory();
    void push (const axg::DisplayFrame& f);
    void setMode (int m) { mode = m; }
    void paint (juce::Graphics&) override;

private:
    std::vector<axg::DisplayFrame> ring;
    int pos = 0, mode = 0;
};

// a transparent area that only shows a tooltip (over painted displays)
struct TipArea : public juce::Component, public juce::SettableTooltipClient
{
    TipArea() { setInterceptsMouseClicks (true, false); }
};

// RHYTHM (2.0): the level of each step, a bar under each step button - drag up / down, double-click = 100 %
class StepLevels : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit StepLevels (juce::AudioProcessorValueTreeState& s);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void setFrom (const juce::MouseEvent&);
    juce::AudioProcessorValueTreeState& apvts;
    std::array<bool, 16> touching {};
};

class GateEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit GateEditor (GateProcessor&);
    ~GateEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void refreshNow() { timerCallback(); }

private:
    class Content : public juce::Component
    {
    public:
        explicit Content (GateEditor& e) : owner (e) {}
        void paint (juce::Graphics&) override;
        void resized() override;
        GateEditor& owner;
    };
    void timerCallback() override;
    void layout();
    void updateVisibility();
    void showPresetMenu();
    void promptSavePreset();

    GateProcessor& proc;
    ui::LookAndFeel lnf;
    Content content { *this };
    juce::TooltipWindow tooltips { this, 700 };

    juce::TextButton presetPrev { "<" }, presetNext { ">" }, presetSave { "SAVE" };
    ui::MenuButton presetName;
    juce::TextButton slotA { "A" }, slotB { "B" }, slotCopy { "COPY" };
    ui::Segmented mode { { "VOCAL", "SHAPE", "RHYTHM" }, ui::col::accent };
    GateHistory history;

    // VOCAL
    ui::Segmented detect { { "SMART", "LEVEL" }, ui::col::teal }, speed { { "LIVE", "FAST", "BEST" }, ui::col::teal };
    ui::Knob sensitivity { "SENSITIVITY", ui::col::teal, 96 }, threshold { "THRESHOLD", ui::col::teal, 96 }, breath { "BREATH", ui::col::teal, 72 };
    ui::Toggle bleed { "BLEED REMOVAL", ui::col::warn, "pill" }, freeze { "FREEZE", ui::col::global, "pill" };
    ui::Knob keyHp { "KEY HP", ui::col::teal, 52 }, keyLp { "KEY LP", ui::col::teal, 52 };
    ui::Toggle keyListen { "KEY LISTEN", ui::col::warn, "pill" };
    TipArea bleedMeterTip;
    // SHAPE
    ui::Knob tail { "TAIL", ui::col::accent, 120 }, hitSens { "HIT SENS", ui::col::accent, 64 }, hitNote { "HIT NOTE", ui::col::accent, 64 };
    // RHYTHM
    std::array<std::unique_ptr<ui::Toggle>, 16> steps;
    StepLevels stepLevels { proc.apvts };
    ui::Choice rate;
    ui::Knob length { "LENGTH", ui::col::accent, 64 }, swing { "SWING", ui::col::accent, 64 }, edge { "EDGE", ui::col::accent, 64 };
    // common
    ui::Knob range { "RANGE", ui::col::global, 64 }, attack { "ATTACK", ui::col::global, 64 }, hold { "HOLD", ui::col::global, 64 },
        release { "RELEASE", ui::col::global, 64 }, output { "OUTPUT", ui::col::global, 64 };
    ui::LevelMeter inMeter { "IN" }, outMeter { "OUT" };

    juce::Rectangle<int> headerArea, mainArea, controlArea, bottomArea, detectLabel, speedLabel, bleedArea, bleedMeterArea, rateLabel, stepArea,
        midiNoteArea;
    std::unique_ptr<juce::AlertWindow> saveDialog;
    bool constructed = false;
    int shownMode = -1;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GateEditor)
};
