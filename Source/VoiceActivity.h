#pragma once
// Axell Gate - voice activity. 3.0: two voice networks agree.
//
// The detector itself (features, background floor, context, room model, loud evidence) is Axell Gate 2.0's, shared
// by the suite in axell-core (axell/dsp/VoiceActivity.h, bit-identical to 2.0). On every frame two networks judge it:
//  - 2.0's network (24 VCTK + 3 ARCTIC voices, level labels), on the inputs it was trained with (Velvet 1.0's
//    sibilance probability);
//  - 3.0's network (111 voices: 108 VCTK + 3 ARCTIC, labels where the forced alignment and the level agree, filled word
//    gaps left out), on its inputs: the same frame with Velvet 3.0's sibilance probability (the same detector frame
//    through Velvet 3.0's network - no second detector).
// Where 2.0's network is sure of a voice (p >= 0.8) it decides alone - the artist's own takes keep 2.0's voice; on the
// doubtful frames a voice needs both networks (the smaller probability) - fewer false openings and far less voice lost
// on voices neither network trained on (research/bench3_gate.py, DECISIONS G23).
// Real-time safe: prepare() allocates; push() never allocates, locks or throws.
#include <axell/dsp/VoiceActivity.h>

#ifdef AXG_VOICE_MODEL3_HEADER
 #include AXG_VOICE_MODEL3_HEADER // research builds: held-out models
#else
 #include "VoiceModel3.h"
#endif

#include <algorithm>
#include <cmath>

namespace axg
{
namespace voice = axell::voice;
namespace vmodel = axell::voice::vamodel;   // 2.0's network (its prior drives the voice / pause model)
using voice::kVadInputs;
using voice::VoiceFrame;
static_assert (vmodel3::kInputs == kVadInputs, "VoiceModel3.h does not match the voice-activity inputs");

class VoiceActivity
{
public:
    void prepare (double fs) { va.prepare (fs); }
    void reset() noexcept { va.reset(); }
    int hopSize() const noexcept { return va.hopSize(); }
    int frameDelay() const noexcept { return va.frameDelay(); }
    const voice::SibilanceDetector& detector() const noexcept { return va.detector(); }

    // feed one mono sample; true when out() holds a new frame (centre = newest - frameDelay())
    bool push (float x) noexcept
    {
        if (! va.push (x))
            return false;
        result = va.out();
        float x3[kVadInputs];
        std::copy (result.x, result.x + kVadInputs, x3);
        x3[18] = voice::SibilanceDetector::probability (va.detector().out().x, voice::kVelvet3Model);
        float pb3 = 0.0f, pv3 = 0.0f, pr3 = 0.0f;
        probabilities3 (x3, pb3, pv3, pr3);
        pv2 = result.pVoice;
        p3 = pv3;
        // where 2.0's network is sure of a voice it decides; elsewhere both must hear a voice (DECISIONS G23)
        const float pv = pv2 >= kSure ? pv2 : std::min (pv2, pv3);
        result.pBack += result.pVoice - pv;       // what is not voice any more counts as background
        result.pVoice = pv;
        return true;
    }

    const VoiceFrame& out() const noexcept { return result; }
    static constexpr float kSure = 0.8f;            // 2.0's voice probability above which 2.0 alone decides
    float voice2() const noexcept { return pv2; }  // each network's own voice probability (research, displays)
    float voice3() const noexcept { return p3; }

    // 3.0's network on its inputs: probabilities of background, voice, breath
    static void probabilities3 (const float* x, float& pb, float& pv, float& pr) noexcept
    {
        float z[kVadInputs];
        for (int i = 0; i < kVadInputs; ++i)
            z[i] = (x[i] - vmodel3::kMu[i]) / vmodel3::kSd[i];
        float o[3] = { vmodel3::kB2[0], vmodel3::kB2[1], vmodel3::kB2[2] };
        for (int j = 0; j < vmodel3::kHidden; ++j)
        {
            float a = vmodel3::kB1[j];
            for (int i = 0; i < kVadInputs; ++i)
                a += z[i] * vmodel3::kW1[i * vmodel3::kHidden + j];
            const float h = std::tanh (a);
            for (int c = 0; c < 3; ++c)
                o[c] += h * vmodel3::kW2[j * 3 + c];
        }
        const float m = std::max (o[0], std::max (o[1], o[2]));
        const float e0 = std::exp (o[0] - m), e1 = std::exp (o[1] - m), e2 = std::exp (o[2] - m), s = e0 + e1 + e2;
        pb = e0 / s;
        pv = e1 / s;
        pr = e2 / s;
    }

private:
    voice::VoiceActivity va;
    VoiceFrame result;
    float pv2 = 0.0f, p3 = 0.0f;
};
} // namespace axg
