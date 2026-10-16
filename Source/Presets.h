#pragma once
// Axell Gate - factory presets (framework independent: the test suite renders and checks every one). Names describe
// the use; no third-party branding.
#include "Engine.h"

#include <array>

namespace axg
{
struct FactoryPreset
{
    const char* category;
    const char* name;
    const char* description;
    Params params;
};

inline Params vocal (int speed, int detect, float range, float breath, float hold, float release, float sens = 0.0f, bool bleed = false,
                     float threshold = -45.0f)
{
    Params p;
    p.mode = kVocal;
    p.speed = speed;
    p.detect = detect;
    p.rangeDb = range;
    p.breathDb = breath;
    p.holdMs = hold;
    p.releaseMs = release;
    p.sensitivity = sens;
    p.bleed = bleed;
    p.thresholdDb = threshold;
    return p;
}

inline Params shape (float tail, float threshold, float range, float attack, float hold, float release, float sensDb = 9.0f)
{
    Params p;
    p.mode = kShape;
    p.tailPct = tail;
    p.thresholdDb = threshold;
    p.rangeDb = range;
    p.attackMs = attack;
    p.holdMs = hold;
    p.releaseMs = release;
    p.sensDb = sensDb;
    return p;
}

inline Params rhythm (uint32_t pattern, int rate, float swing, float edge, float range, int steps = 16)
{
    Params p;
    p.mode = kRhythm;
    p.pattern = pattern;
    p.steps = steps;
    p.rate = rate;
    p.swingPct = swing;
    p.edgeMs = edge;
    p.rangeDb = range;
    return p;
}

inline Params withLevels (Params p, std::array<float, 4> beat) // RHYTHM: the same 4 step levels in every beat
{
    for (int k = 0; k < 16; ++k)
        p.stepLevel[(size_t) k] = beat[(size_t) (k % 4)];
    return p;
}

inline Params withKey (Params p, float hpHz, float lpHz)
{
    p.keyHpHz = hpHz;
    p.keyLpHz = lpHz;
    return p;
}

inline Params withNote (Params p, int note)
{
    p.midiNote = note;
    return p;
}

constexpr int kNumFactoryPresets = 25;

inline std::array<FactoryPreset, kNumFactoryPresets> factoryPresets()
{
    return { {
        { "VOCALS", "Init", "Smart vocal gate: opens on the voice, breaths at -12 dB, BEST look-ahead.", vocal (kBest, kSmart, -60, -12, 30, 50) },
        { "VOCALS", "Rap Lead - Clean Gaps", "Silence between the bars; breaths lowered, every word start intact.", vocal (kBest, kSmart, -60, -12, 30, 50) },
        { "VOCALS", "Rap Lead - Kill Beat Bleed", "Route the beat into the side-chain: the headphone bleed is removed in the gaps AND under the voice.",
          vocal (kBest, kSmart, -60, -12, 30, 50, 0.0f, true) },
        { "VOCALS", "Ad-Libs & Doubles", "Tighter: short hold, breaths removed - layers stay out of the way.", vocal (kBest, kSmart, -70, -40, 5, 40, -20.0f) },
        { "VOCALS", "Breath Remover", "Keeps every word, takes the breaths out completely.", vocal (kBest, kSmart, -20, -80, 30, 50) },
        { "VOCALS", "Natural Singing", "Gentle: only 12 dB of reduction, slow release, breaths kept - for sung takes with sustained notes.",
          vocal (kBest, kSmart, -12, 0, 115, 200, 20.0f) },
        { "VOCALS", "Podcast & Voice-Over", "Speech: room tone down 30 dB, breaths half way.", vocal (kBest, kSmart, -30, -10, 45, 80) },
        { "VOCALS", "Live Tracking (Zero Latency)", "For monitoring while you record: a classic level gate with no latency. Set THRESHOLD above the room.",
          vocal (kLive, kLevel, -40, -12, 80, 100, 0.0f, false, -45.0f) },
        // SHAPE (programmed drums)
        { "DRUMS", "Tight Kick", "Kick tail at 50 %: same shape, half as long - room for the 808.", shape (50, -50, -80, 1.0f, 0, 200) },
        { "DRUMS", "Short Snare", "Snare tail at 40 %: snappy, no ring.", shape (40, -50, -80, 0.5f, 0, 150) },
        { "DRUMS", "808 Tamer", "808 tail at 60 %: keeps the note, stops it from smearing into the next one.", shape (60, -50, -80, 2.0f, 0, 300) },
        { "DRUMS", "Gated Snare (80s)", "The classic: the snare's reverb cut to a short, hard tail.", shape (25, -40, -80, 0.5f, 60, 30) },
        { "DRUMS", "Punchy Toms", "Toms at 55 %: body and attack, less boom.", shape (55, -50, -80, 1.0f, 0, 200) },
        { "DRUMS", "Sample Cleanup", "Natural tails, but the noise between the hits gated out.", shape (100, -45, -80, 1.0f, 40, 120) },
        { "DRUMS", "Hats - Super Tight", "Hi-hats at 30 %: crisp ticks.", shape (30, -50, -80, 0.2f, 0, 60, 6.0f) },
        // RHYTHM
        { "RHYTHM", "Trance Gate 16ths", "Every second 16th open: the classic pumping chop.", rhythm (0x5555u, 2, 0, 3, -80) },
        { "RHYTHM", "Stutter 32nds", "Fast 32nd-note stutter, soft edges.", rhythm (0x5555u, 3, 0, 2, -80) },
        { "RHYTHM", "Half-Time Chop", "Long and short: a broken half-time feel on 16ths.", rhythm (0x2D2Du, 2, 0, 4, -80) },
        { "RHYTHM", "Swing Gate", "16ths with 50 % swing for bouncy hip-hop movement.", rhythm (0x7777u, 2, 50, 3, -60) },
        { "RHYTHM", "Triplet Pulse", "Triplet 8ths, every third step closed (12-step bar).", rhythm (0x0DB3u, 4, 0, 4, -60, 12) },
        { "RHYTHM", "Soft Tremolo 8ths", "Gentle 8th-note pulse, wide edges, only 12 dB deep.", rhythm (0x5555u, 1, 0, 30, -12) },
        // 2.0 (appended: the 1.0 program numbers stay)
        { "DRUMS", "Kick Trigger (MIDI)", "Audio untouched; a MIDI note per kick (C1, velocity from the hit) to layer a sample.",
          withNote (shape (100, -40, 0, 1.0f, 0, 100), 36) },
        { "RHYTHM", "Ducking Pump", "Every 16th open, rising through each beat: the side-chain pump without a side-chain.",
          withLevels (rhythm (0xFFFFu, 2, 0, 8, -80), { 0.08f, 0.45f, 0.8f, 1.0f }) },
        { "RHYTHM", "Accent Chop", "16ths, full on the beat, quieter between: a chop that keeps the groove.",
          withLevels (rhythm (0xFFFFu, 2, 0, 2, -80), { 1.0f, 0.35f, 0.6f, 0.35f }) },
        { "VOCALS", "Live Tracking - Boomy Room", "Zero-latency level gate keyed above 150 Hz: rumble and bass in the room do not open it.",
          withKey (vocal (kLive, kLevel, -40, -12, 80, 100, 0.0f, false, -45.0f), 150.0f, 20000.0f) },
    } };
}
} // namespace axg
