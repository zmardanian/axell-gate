#include "PluginEditor.h"

#include <cmath>

using namespace ui;

namespace
{
const juce::Identifier kUiWidth ("uiWidth");
namespace tip
{
const char* mode = "VOCAL: a gate that recognises the voice (not just its level). SHAPE: shortens drum tails without "
                   "clicks or kinks. RHYTHM: tempo-synced gate patterns.";
const char* detect = "SMART: a trained voice detector decides - quiet word starts, s and f stay in, bleed and room noise "
                     "stay out, no threshold to set. LEVEL: the classic threshold gate.";
const char* speed = "How far ahead the gate looks. BEST (67 ms latency): the cleanest result. FAST (42 ms). LIVE (0 ms): for "
                    "monitoring while recording - a classic level gate.";
const char* sensitivity = "SMART: + opens more readily (whispered ad-libs), - stays closed on anything doubtful.";
const char* threshold = "The level that opens the gate (LEVEL detection) or that a drum hit must reach (SHAPE).";
const char* breath = "How loud breaths stay: 0 dB untouched, -12 dB tamed (default), fully left = removed.";
const char* bleed = "Removes the beat that leaks from your headphones into the mic - in the gaps AND under the voice. Route "
                    "the beat's track into this plugin's side-chain input (in REAPER: the beat track's send to this "
                    "track, channels 3/4, and pin 3/4 to the side-chain). It learns the leak by itself in a few seconds, "
                    "and what it learned is saved with the project.";
const char* freeze = "Keep the bleed path as learned and stop learning.";
const char* bleedMeter = "GAPS: how much quieter the mic is where the bleed is plain (a conservative reading). FIT: how well the learned leak matches "
                         "(below 100 % it is scaled down, so it can never add bleed). BEAT: the level on the side-chain.";
const char* keyHp = "LEVEL detection listens through this high-pass: rumble and bass below it do not open the gate.";
const char* keyLp = "LEVEL detection listens through this low-pass: hiss and cymbals above it do not open the gate.";
const char* keyListen = "Hear what the LEVEL detection hears (the key, filtered) instead of the output.";
const char* hitNote = "SHAPE sends this MIDI note for every hit it detects, velocity from the hit's level, exactly at the hit "
                      "as you hear it - layer a sample, or drive a synth from the drums.";
const char* stepLevels = "The level of each open step: drag up or down, double-click for 100 %.";
const char* tail = "How long the drum's tail is: 100 % natural, 50 % half as long. The tail keeps its shape (it just fades "
                   "faster) - no kink where a release would start, no click, even on an 808.";
const char* hitSens = "How sharp a rise counts as a new hit: higher = only the clear hits.";
const char* rate = "Step length of the pattern.";
const char* length = "Pattern length in steps (12 for triplet bars).";
const char* swing = "Delays every second step: groove.";
const char* edge = "How soft each step's edges are (ms): short = hard chop, long = smooth pulse.";
const char* range = "How far the gate closes: -80 dB silence, -12 dB a gentle dip.";
const char* attack = "How fast the gate opens. With look-ahead the opening ends exactly at the onset.";
const char* hold = "How long the gate stays open after the sound (after the last voice frame in VOCAL SMART).";
const char* release = "How fast the gate closes.";
const char* output = "Output volume.";
const char* history = "The last seconds: input (grey), output (white), the gate's gain from the top (orange). VOCAL: the "
                      "voice detector's confidence (teal strip) and breaths held at the BREATH level (amber). SHAPE: a tick "
                      "for every detected hit (and its MIDI note).";
} // namespace tip
} // namespace

//==============================================================================
GateHistory::GateHistory() : ring (2000) {}

void GateHistory::push (const axg::DisplayFrame& f)
{
    ring[(size_t) pos] = f;
    pos = (pos + 1) % (int) ring.size();
}

void GateHistory::paint (juce::Graphics& g)
{
    using namespace juce;
    const auto r = getLocalBounds().toFloat();
    g.setColour (col::well);
    g.fillRoundedRectangle (r, 10.0f);
    const auto plot = r.reduced (12.0f, 10.0f).withTrimmedLeft (24.0f).withTrimmedBottom (mode == axg::kVocal ? 20.0f : 6.0f);
    const float lo = -72.0f, hi = 0.0f;
    auto yL = [&] (float d) { return plot.getBottom() - (jlimit (lo, hi, d) - lo) / (hi - lo) * plot.getHeight(); };
    auto yG = [&] (float d) { return plot.getY() + jlimit (0.0f, 80.0f, -d) / 80.0f * plot.getHeight() * 0.5f; };
    g.setFont (ui::font (10.0f));
    for (int k = 1; k < 6; ++k)
    {
        const float d = -12.0f * (float) k;
        g.setColour (col::grid);
        g.drawHorizontalLine ((int) yL (d), plot.getX(), plot.getRight());
        g.setColour (col::faint);
        g.drawText (String ((int) d), Rectangle<float> (r.getX() + 4.0f, yL (d) - 6.0f, 28.0f, 12.0f), Justification::right);
    }
    const int n = (int) plot.getWidth(), N = (int) ring.size();
    const float per = (float) N / (float) n;
    Path gp;
    for (int i = 0; i < n; ++i)
    {
        const int a = (int) (i * per), b = std::max (a + 1, (int) ((i + 1) * per));
        float in = -120.0f, out = -120.0f, gain = 0.0f, pv = 0.0f;
        bool breath = false, hit = false;
        for (int k = a; k < b; ++k)
        {
            const auto& f = ring[(size_t) ((pos + k) % N)];
            in = std::max (in, f.inDb);
            out = std::max (out, f.outDb);
            gain = std::min (gain, f.gainDb);
            pv = std::max (pv, f.pVoice);
            breath = breath || f.state == 1;
            hit = hit || f.hit;
        }
        const float x = plot.getX() + (float) i;
        if (in > lo)
        {
            g.setColour (col::dim.withAlpha (0.35f));
            g.drawVerticalLine ((int) x, yL (in), plot.getBottom());
        }
        if (out > lo)
        {
            g.setColour (col::text.withAlpha (0.55f));
            g.drawVerticalLine ((int) x, yL (out), plot.getBottom());
        }
        if (i == 0) gp.startNewSubPath (x, yG (gain)); else gp.lineTo (x, yG (gain));
        if (mode == axg::kVocal)
        {
            g.setColour (breath ? col::tube : col::teal.withAlpha (0.15f + 0.85f * pv));
            g.drawVerticalLine ((int) x, plot.getBottom() + 6.0f, plot.getBottom() + 16.0f);
        }
        if (mode == axg::kShape && hit)
        {
            g.setColour (col::accent);
            g.fillRect (x - 1.0f, plot.getY() - 6.0f, 2.0f, 8.0f);
        }
    }
    g.setColour (col::accent);
    g.strokePath (gp, PathStrokeType (1.8f));
    g.setFont (ui::caps (9.5f, 1));
    g.setColour (col::faint);
    g.drawText (mode == axg::kVocal ? "VOICE" : "", Rectangle<float> (r.getX() + 2.0f, plot.getBottom() + 4.0f, 32.0f, 12.0f), Justification::left);
    g.drawText ("IN  /  OUT  /  GAIN", Rectangle<float> (plot.getRight() - 200.0f, plot.getY(), 196.0f, 12.0f), Justification::right);
}

//==============================================================================
StepLevels::StepLevels (juce::AudioProcessorValueTreeState& s) : apvts (s) { setRepaintsOnMouseActivity (false); }

void StepLevels::paint (juce::Graphics& g)
{
    const float w = (float) getWidth() / 16.0f;
    const int len = juce::roundToInt (apvts.getRawParameterValue ("steps")->load());
    for (int k = 0; k < 16; ++k)
    {
        const auto cell = juce::Rectangle<float> ((float) k * w + 2.0f, 0.0f, w - 4.0f, (float) getHeight());
        const bool on = apvts.getRawParameterValue ("step" + juce::String (k + 1))->load() > 0.5f && k < len;
        const float lv = apvts.getRawParameterValue ("level" + juce::String (k + 1))->load() * 0.01f;
        g.setColour (ui::col::well);
        g.fillRoundedRectangle (cell, 3.0f);
        g.setColour ((on ? ui::col::accent : ui::col::faint).withAlpha (on ? 0.9f : 0.35f));
        g.fillRoundedRectangle (cell.withTrimmedTop (cell.getHeight() * (1.0f - lv)), 3.0f);
    }
}

void StepLevels::setFrom (const juce::MouseEvent& e)
{
    const int k = juce::jlimit (0, 15, (int) (e.position.x / ((float) getWidth() / 16.0f)));
    auto* p = apvts.getParameter ("level" + juce::String (k + 1));
    if (! touching[(size_t) k])
    {
        p->beginChangeGesture();
        touching[(size_t) k] = true;
    }
    const float v = juce::jlimit (0.0f, 1.0f, 1.0f - e.position.y / (float) juce::jmax (1, getHeight()));
    p->setValueNotifyingHost (p->convertTo0to1 (100.0f * v));
    repaint();
}

void StepLevels::mouseDown (const juce::MouseEvent& e) { setFrom (e); }
void StepLevels::mouseDrag (const juce::MouseEvent& e) { setFrom (e); }

void StepLevels::mouseUp (const juce::MouseEvent&)
{
    for (int k = 0; k < 16; ++k)
        if (touching[(size_t) k])
        {
            apvts.getParameter ("level" + juce::String (k + 1))->endChangeGesture();
            touching[(size_t) k] = false;
        }
}

void StepLevels::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int k = juce::jlimit (0, 15, (int) (e.position.x / ((float) getWidth() / 16.0f)));
    auto* p = apvts.getParameter ("level" + juce::String (k + 1));
    p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 (100.0f));
    p->endChangeGesture();
    repaint();
}

//==============================================================================
GateEditor::GateEditor (GateProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    const int savedWidth = (int) proc.apvts.state.getProperty (kUiWidth, gui::kWidth);
    setLookAndFeel (&lnf);
    addAndMakeVisible (content);
    mode.bind (*proc.apvts.getParameter ("mode"), tip::mode);
    detect.bind (*proc.apvts.getParameter ("detect"), tip::detect);
    speed.bind (*proc.apvts.getParameter ("speed"), tip::speed);
    sensitivity.bind (proc.apvts, "sensitivity", tip::sensitivity);
    threshold.bind (proc.apvts, "threshold", tip::threshold);
    breath.bind (proc.apvts, "breath", tip::breath);
    bleed.bind (proc.apvts, "bleed", tip::bleed);
    freeze.bind (proc.apvts, "bleedFreeze", tip::freeze);
    keyHp.bind (proc.apvts, "keyHp", tip::keyHp);
    keyLp.bind (proc.apvts, "keyLp", tip::keyLp);
    keyListen.bind (proc.apvts, "keyListen", tip::keyListen);
    hitNote.bind (proc.apvts, "midiNote", tip::hitNote);
    stepLevels.setTooltip (tip::stepLevels);
    bleedMeterTip.setTooltip (tip::bleedMeter);
    tail.bind (proc.apvts, "tail", tip::tail);
    hitSens.bind (proc.apvts, "sensDb", tip::hitSens);
    rate.bind (proc.apvts, "rate", tip::rate);
    length.bind (proc.apvts, "steps", tip::length);
    swing.bind (proc.apvts, "swing", tip::swing);
    edge.bind (proc.apvts, "edge", tip::edge);
    range.bind (proc.apvts, "range", tip::range);
    attack.bind (proc.apvts, "attack", tip::attack);
    hold.bind (proc.apvts, "hold", tip::hold);
    release.bind (proc.apvts, "release", tip::release);
    output.bind (proc.apvts, "output", tip::output);
    history.setTooltip (tip::history);
    for (int k = 0; k < 16; ++k)
    {
        steps[(size_t) k] = std::make_unique<ui::Toggle> (juce::String (k + 1), col::accent, "pill");
        steps[(size_t) k]->bind (proc.apvts, "step" + juce::String (k + 1), "Step " + juce::String (k + 1) + ": open or closed");
        content.addChildComponent (*steps[(size_t) k]);
    }
    for (auto* c : std::initializer_list<juce::Component*> { &presetPrev, &presetNext, &presetName, &presetSave, &slotA, &slotB, &slotCopy,
                                                            &mode, &history, &range, &attack, &hold, &release, &output, &inMeter, &outMeter })
        content.addAndMakeVisible (c);
    for (auto* c : std::initializer_list<juce::Component*> { &detect, &speed, &sensitivity, &threshold, &breath, &bleed, &freeze, &tail,
                                                            &hitSens, &rate, &length, &swing, &edge, &keyHp, &keyLp, &keyListen,
                                                            &hitNote, &stepLevels, &bleedMeterTip })
        content.addChildComponent (c);
    for (auto* b : { &slotA, &slotB })
        b->setColour (juce::TextButton::buttonOnColourId, col::global);
    presetPrev.onClick = [this] { proc.setCurrentProgram ((proc.getCurrentProgram() + proc.getNumPrograms() - 1) % proc.getNumPrograms()); };
    presetNext.onClick = [this] { proc.setCurrentProgram ((proc.getCurrentProgram() + 1) % proc.getNumPrograms()); };
    presetName.onClick = [this] { showPresetMenu(); };
    presetSave.onClick = [this] { promptSavePreset(); };
    slotA.onClick = [this] { proc.selectSlot (0); };
    slotB.onClick = [this] { proc.selectSlot (1); };
    slotCopy.onClick = [this] { proc.copyActiveToOther(); };
    presetName.setTooltip ("Presets for vocals, drums and rhythm. Your own presets live in ~/.local/share/AxellGate/Presets.");
    slotA.setTooltip ("A/B: two complete settings to compare.");
    slotB.setTooltip ("A/B: two complete settings to compare.");
    slotCopy.setTooltip ("Copy the current slot to the other one.");

    setResizable (true, true);
    setResizeLimits (gui::kWidth * 3 / 4, gui::kHeight * 3 / 4, gui::kWidth * 2, gui::kHeight * 2);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) gui::kWidth / gui::kHeight);
    const int w = juce::jlimit (gui::kWidth * 3 / 4, gui::kWidth * 2, savedWidth);
    setSize (w, juce::roundToInt (w * (double) gui::kHeight / gui::kWidth));
    constructed = true;
    timerCallback();
    startTimerHz (30);
}

GateEditor::~GateEditor()
{
    stopTimer();
    saveDialog.reset();
    setLookAndFeel (nullptr);
}

void GateEditor::paint (juce::Graphics& g) { g.fillAll (col::bg); }

void GateEditor::resized()
{
    content.setBounds (0, 0, gui::kWidth, gui::kHeight);
    content.setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) gui::kWidth));
    if (constructed)
        proc.apvts.state.setProperty (kUiWidth, getWidth(), nullptr);
}

void GateEditor::Content::resized() { owner.layout(); }

void GateEditor::layout()
{
    using juce::Rectangle;
    const int m = 16;
    headerArea = { 0, 0, gui::kWidth, 58 };
    {
        auto pb = Rectangle<int> (360, 14, 400, 30);
        presetPrev.setBounds (pb.removeFromLeft (30));
        pb.removeFromLeft (4);
        presetSave.setBounds (pb.removeFromRight (62));
        pb.removeFromRight (4);
        presetNext.setBounds (pb.removeFromRight (30));
        pb.removeFromRight (4);
        presetName.setBounds (pb);
        auto ab = Rectangle<int> (gui::kWidth - m - 160, 14, 160, 30);
        slotA.setBounds (ab.removeFromLeft (34));
        ab.removeFromLeft (4);
        slotB.setBounds (ab.removeFromLeft (34));
        ab.removeFromLeft (8);
        slotCopy.setBounds (ab);
    }
    mode.setBounds (m, 70, 420, 32);
    mainArea = { m, 112, gui::kWidth - 2 * m, 220 };
    history.setBounds (mainArea);
    controlArea = { m, 344, gui::kWidth - 2 * m, 214 };
    bottomArea = { m, 568, gui::kWidth - 2 * m, gui::kHeight - 568 - m };
    // VOCAL
    {
        auto a = controlArea.reduced (14, 12);
        auto left = a.removeFromLeft (250);
        detectLabel = left.removeFromTop (16);
        detect.setBounds (left.removeFromTop (28));
        left.removeFromTop (12);
        speedLabel = left.removeFromTop (16);
        speed.setBounds (left.removeFromTop (28));
        a.removeFromLeft (16);
        auto big = a.removeFromLeft (150);
        sensitivity.setBounds (big);
        threshold.setBounds (big);
        a.removeFromLeft (8);
        breath.setBounds (a.removeFromLeft (110));
        a.removeFromLeft (16);
        bleedArea = a.removeFromLeft (300);
        auto b = bleedArea.reduced (12, 10).withTrimmedTop (24);
        bleed.setBounds (b.removeFromTop (32));
        b.removeFromTop (6);
        freeze.setBounds (b.removeFromTop (26).removeFromLeft (100));
        b.removeFromTop (6);
        bleedMeterArea = b;
        bleedMeterTip.setBounds (b);
        a.removeFromLeft (8);
        auto key = a;
        auto knobs = key.removeFromTop (key.getHeight() - 34);
        keyHp.setBounds (knobs.removeFromLeft (knobs.getWidth() / 2));
        keyLp.setBounds (knobs);
        keyListen.setBounds (key.withSizeKeepingCentre (key.getWidth() - 16, 28));
    }
    // SHAPE
    {
        auto a = controlArea.reduced (14, 12);
        tail.setBounds (a.removeFromLeft (190));
        a.removeFromLeft (12);
        hitSens.setBounds (a.removeFromLeft (110));
        a.removeFromLeft (12);
        a.removeFromLeft (110); // threshold shares the VOCAL knob position in SHAPE (re-positioned in updateVisibility)
        a.removeFromLeft (12);
        hitNote.setBounds (a.removeFromLeft (110));
        a.removeFromLeft (12);
        midiNoteArea = a.removeFromLeft (300);
    }
    // RHYTHM
    {
        auto a = controlArea.reduced (14, 12);
        stepArea = a.removeFromTop (66);
        const int w = stepArea.getWidth() / 16;
        for (int k = 0; k < 16; ++k)
            steps[(size_t) k]->setBounds (stepArea.getX() + k * w + 2, stepArea.getY() + 2, w - 4, 28);
        stepLevels.setBounds (stepArea.getX(), stepArea.getY() + 34, w * 16, 30);
        a.removeFromTop (8);
        auto row = a;
        rateLabel = row.removeFromLeft (160).removeFromTop (16);
        rate.setBounds (rateLabel.getX(), rateLabel.getBottom() + 4, 150, 30);
        row.removeFromLeft (10);
        length.setBounds (row.removeFromLeft (110));
        swing.setBounds (row.removeFromLeft (110));
        edge.setBounds (row.removeFromLeft (110));
    }
    // common, bottom
    {
        auto a = bottomArea.reduced (14, 8);
        outMeter.setBounds (a.removeFromRight (44));
        a.removeFromRight (6);
        inMeter.setBounds (a.removeFromRight (44));
        a.removeFromRight (16);
        output.setBounds (a.removeFromRight (96));
        a.removeFromRight (16);
        for (auto* k : { &range, &attack, &hold, &release })
            k->setBounds (a.removeFromLeft (96));
    }
    updateVisibility();
}

void GateEditor::updateVisibility()
{
    const auto p = proc.readParams();
    shownMode = p.mode;
    history.setMode (p.mode);
    const bool v = p.mode == axg::kVocal, s = p.mode == axg::kShape, r = p.mode == axg::kRhythm;
    for (auto* c : std::initializer_list<juce::Component*> { &detect, &speed, &breath, &bleed, &freeze, &bleedMeterTip })
        c->setVisible (v);
    const bool levelDetect = v && (p.detect == axg::kLevel || p.speed == axg::kLive);
    sensitivity.setVisible (v && ! levelDetect);
    threshold.setVisible (levelDetect || s);
    for (auto* c : std::initializer_list<juce::Component*> { &keyHp, &keyLp, &keyListen })
        c->setVisible (levelDetect);
    hitNote.setVisible (s);
    stepLevels.setVisible (r);
    if (s)
        threshold.setBounds (controlArea.reduced (14, 12).withTrimmedLeft (190 + 12 + 110 + 12).removeFromLeft (110));
    else
    {
        auto a = controlArea.reduced (14, 12).withTrimmedLeft (250 + 16);
        threshold.setBounds (a.removeFromLeft (150));
    }
    tail.setVisible (s);
    hitSens.setVisible (s);
    for (auto& st : steps)
        st->setVisible (r);
    for (auto* c : std::initializer_list<juce::Component*> { &rate, &length, &swing, &edge })
        c->setVisible (r);
    attack.setVisible (! r);
    hold.setVisible (! r);
    release.setVisible (! r);
    content.repaint();
}

void GateEditor::Content::paint (juce::Graphics& g)
{
    using namespace juce;
    auto& e = owner;
    g.fillAll (col::bg);
    g.setColour (col::header);
    g.fillRect (e.headerArea);
    g.setColour (col::grid);
    g.drawHorizontalLine (e.headerArea.getBottom() - 1, 0.0f, (float) gui::kWidth);
    g.setColour (col::text);
    g.setFont (ui::caps (21.0f, 2));
    g.drawText ("AXELL GATE", 18, 8, 320, 26, Justification::centredLeft);
    g.setFont (ui::caps (10.0f, 1));
    g.setColour (col::dim);
    g.drawText ("SMART VOCAL GATE  +  DRUM SHAPER  +  RHYTHM", 19, 33, 340, 14, Justification::centredLeft);
    auto panel = [&g] (Rectangle<int> a, Colour accent)
    {
        const auto r = a.toFloat();
        g.setGradientFill (ColourGradient (col::panelTop, 0, r.getY(), col::panel, 0, r.getBottom(), false));
        g.fillRoundedRectangle (r, 10.0f);
        g.setColour (accent.withAlpha (0.4f));
        g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.2f);
    };
    const auto p = e.proc.readParams();
    panel (e.controlArea, p.mode == axg::kVocal ? col::teal : col::accent);
    panel (e.bottomArea, col::global);
    g.setColour (col::dim);
    g.setFont (ui::caps (10.5f, 1));
    if (p.mode == axg::kVocal)
    {
        g.drawText ("DETECTION", e.detectLabel, Justification::centredLeft);
        g.drawText ("SPEED", e.speedLabel, Justification::centredLeft);
        const auto r = e.bleedArea.toFloat();
        g.setColour (col::well);
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (col::dim);
        g.drawText ("BEAT BLEED (SIDE-CHAIN)", e.bleedArea.reduced (12, 8).removeFromTop (16), Justification::centredLeft);
        const bool sc = e.proc.sidechainConnected();
        auto& dsp = e.proc.engine();
        auto st = e.freeze.getBounds().withLeft (e.freeze.getRight() + 10).withRight (e.bleedArea.getRight() - 12);
        g.setFont (ui::font (11.0f));
        g.setColour (sc ? col::ok : col::faint);
        g.drawFittedText (! sc ? "No beat on the side-chain (inputs 3/4)."
                               : ! p.bleed ? "Beat connected."
                               : dsp.bleedLearned() ? "Learned - saved with the project." : "Learning...",
                          st, Justification::centredLeft, 2);
        // the meter: GAPS (dB quieter), FIT, BEAT
        const bool live = sc && p.bleed;
        auto mr = e.bleedMeterArea;
        const int rowH = mr.getHeight() / 3;
        auto row = [&] (const char* name, float frac, const String& val, Colour c)
        {
            auto rr = mr.removeFromTop (rowH).reduced (0, 3);
            g.setColour (col::dim);
            g.setFont (ui::caps (9.5f, 1));
            g.drawText (name, rr.removeFromLeft (44), Justification::centredLeft);
            auto vr = rr.removeFromRight (70);
            g.setFont (ui::font (11.0f));
            g.setColour (live ? col::text : col::faint);
            g.drawText (live ? val : String ("-"), vr, Justification::centredRight);
            const auto bar = rr.reduced (4, 3).toFloat();
            g.setColour (col::track);
            g.fillRoundedRectangle (bar, 3.0f);
            if (live)
            {
                g.setColour (c);
                g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * jlimit (0.0f, 1.0f, frac)), 3.0f);
            }
        };
        const float gap = dsp.bleedGapDb(), fit = dsp.bleedFit(), beat = dsp.bleedBeatDb();
        row ("GAPS", gap / 40.0f, "-" + String (jmax (0.0f, gap), 1) + " dB", col::warn);
        row ("FIT", fit, String (roundToInt (100.0f * fit)) + " %", fit > 0.9f ? col::ok : col::warn);
        row ("BEAT", (beat + 60.0f) / 60.0f, beat > -119.0f ? String (beat, 1) + " dBFS" : String ("-"), col::global);
        if (p.detect == axg::kLevel || p.speed == axg::kLive)
        {
            g.setColour (col::dim);
            g.setFont (ui::caps (10.5f, 1));
            g.drawText ("KEY", e.keyHp.getBounds().withHeight (14).translated (0, -2), Justification::centredLeft);
        }
    }
    else if (p.mode == axg::kShape)
    {
        g.setColour (col::dim);
        g.setFont (ui::caps (10.5f, 1));
        g.drawText ("MIDI OUT", e.midiNoteArea.removeFromTop (16), Justification::centredLeft);
        g.setFont (ui::font (11.5f));
        g.setColour (col::faint);
        g.drawFittedText ("A note for every hit, velocity from the hit, exactly where you hear it. In REAPER: route this "
                          "track's MIDI output (or a send) to an instrument track.",
                          e.midiNoteArea.withTrimmedTop (20).removeFromTop (60), Justification::topLeft, 4);
    }
    else if (p.mode == axg::kRhythm)
        g.drawText ("RATE", e.rateLabel, Justification::centredLeft);
    g.setFont (ui::font (11.0f));
    g.setColour (col::faint);
    g.drawText ("latency " + String (e.proc.getLatencySamples()) + " samples ("
                    + String (1000.0 * e.proc.getLatencySamples() / std::max (1.0, e.proc.engine().sampleRate()), 1) + " ms, compensated)",
                e.bottomArea.reduced (14, 6).withTrimmedLeft (4 * 96 + 20).withTrimmedRight (230), Justification::centredLeft);
}

void GateEditor::timerCallback()
{
    axg::DisplayFrame f;
    float in = -120.0f, out = -120.0f;
    bool any = false;
    while (proc.engine().popDisplay (f))
    {
        history.push (f);
        in = std::max (in, f.inDb);
        out = std::max (out, f.outDb);
        any = true;
    }
    if (any)
        history.repaint();
    auto lin = [] (float db) { return db > -119.0f ? std::pow (10.0f, db / 20.0f) : 0.0f; };
    inMeter.push (lin (in), lin (in), 2);
    outMeter.push (lin (out), lin (out), 2);
    const auto p = proc.readParams();
    if (p.mode != shownMode || sensitivity.isVisible() != (p.mode == axg::kVocal && p.detect == axg::kSmart && p.speed != axg::kLive))
        updateVisibility();
    presetName.setButtonText (proc.getPresetName());
    slotA.setToggleState (proc.getActiveSlot() == 0, juce::dontSendNotification);
    slotB.setToggleState (proc.getActiveSlot() == 1, juce::dontSendNotification);
    slotCopy.setButtonText (proc.getActiveSlot() == 0 ? "COPY A>B" : "COPY B>A");
    content.repaint (bottomArea);
    content.repaint (bleedArea);
    if (stepLevels.isVisible())
        stepLevels.repaint();
}

void GateEditor::showPresetMenu()
{
    juce::PopupMenu menu, user;
    const auto list = axg::factoryPresets();
    for (const char* section : { "VOCALS", "DRUMS", "RHYTHM" }) // grouped (2.0 presets are appended at the end of the list)
    {
        menu.addSectionHeader (section);
        for (int i = 0; i < (int) list.size(); ++i)
            if (juce::String (list[(size_t) i].category) == section)
                menu.addItem (1 + i, list[(size_t) i].name);
    }
    auto files = GateProcessor::userPresetFolder().findChildFiles (juce::File::findFiles, false, "*.axgpreset");
    files.sort();
    for (int i = 0; i < files.size(); ++i)
        user.addItem (1000 + i, files[i].getFileNameWithoutExtension());
    menu.addSeparator();
    menu.addSectionHeader ("USER");
    if (files.isEmpty())
        menu.addItem (-1, "(no user presets yet)", false);
    else
        menu.addSubMenu ("Your presets", user);
    juce::Component::SafePointer<GateEditor> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetName), [safe, files] (int id)
    {
        if (safe == nullptr || id <= 0)
            return;
        if (id >= 1000)
            safe->proc.loadUserPreset (files[id - 1000]);
        else
            safe->proc.setCurrentProgram (id - 1);
    });
}

void GateEditor::promptSavePreset()
{
    using namespace juce;
    saveDialog = std::make_unique<AlertWindow> ("Save preset", "Name for this preset:", MessageBoxIconType::NoIcon, this);
    auto* w = saveDialog.get();
    w->setLookAndFeel (&lnf);
    w->addTextEditor ("name", proc.getPresetName(), {});
    w->addButton ("Save", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    Component::SafePointer<GateEditor> safe (this);
    w->enterModalState (true, ModalCallbackFunction::create ([safe] (int result)
    {
        if (safe == nullptr || safe->saveDialog == nullptr)
            return;
        const auto name = safe->saveDialog->getTextEditorContents ("name").trim();
        if (result == 1 && name.isNotEmpty() && ! safe->proc.saveUserPreset (name))
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Save preset",
                                              "Could not write to " + GateProcessor::userPresetFolder().getFullPathName());
        MessageManager::callAsync ([safe] { if (safe != nullptr) safe->saveDialog.reset(); });
    }), false);
}
