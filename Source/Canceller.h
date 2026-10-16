#pragma once
// Axell Gate - BLEED canceller (framework independent, no JUCE).
//
// The beat on the side-chain (L, R: the headphone feed) leaks into the vocal mic. A partitioned-block frequency-domain
// adaptive Kalman filter (Enzner & Vary, Signal Processing 86, 2006; partitioned: Kuech, Mabande & Enzner, IEEE/ACM
// TASLP 22, 2014), diagonal per bin, two references, learns that leak path and subtracts it - in the pauses AND under
// the voice. Per bin the Kalman gain is the optimal step size: where the voice, a breath or the room dominates, the
// bin barely adapts (no double-talk detector). One partition per reference is gradient-constrained per block, in turn
// (the efficient form of the paper); the others adapt unconstrained in between.
// Safety: the estimate is scaled by its projection on the mic (<mic, est> / <est, est>, smoothed, limited to 0..1), so
// a wrong estimate can never add bleed. FREEZE stops learning (the path stays as learned).
// Latency: one block + `lead` (the mic is delayed so bleed that arrives slightly before the reference is modelled).
// Persistence (2.0): exportPath() / importPath() carry what was learned - the leak path as impulse responses, its
// uncertainty per partition and the safety statistics - so a project reopens already cancelling.
// Real-time safe: prepare() allocates; process() never allocates, locks or throws.
#include <axell/dsp/Fft.h>
#include <axell/dsp/Math.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace axg
{
class Canceller
{
public:
    static constexpr int kRefs = 2;
    static constexpr double kTapsMs = 60.0, kLeadMs = 8.0, kA = 0.9999, kInitVar = 1.0e-3, kPsiFloor = 0.05;

    void prepare (double fs)
    {
        B = fs > 64000.0 ? 512 : 256;
        N = 2 * B;
        K = B + 1;
        P = (int) std::ceil (kTapsMs * 1.0e-3 * fs / B);
        lead = (int) std::lround (kLeadMs * 1.0e-3 * fs);
        fft.prepare (N);
        for (auto& w : W) w.assign ((size_t) (P * K), {});
        for (auto& x : X) x.assign ((size_t) (P * K), {});
        for (auto& v : Pv) v.assign ((size_t) (P * K), kInitVar);
        for (auto& b : xbuf) b.assign ((size_t) N, 0.0f);
        PsiS.assign ((size_t) K, 0.0);
        E.assign ((size_t) K, {});
        Y.assign ((size_t) K, {});
        tmpT.assign ((size_t) N, 0.0f);
        tmpF.assign ((size_t) N + 2, 0.0f);
        micRing.assign ((size_t) (lead + 2 * B + 8), 0.0f);
        inBlock.assign ((size_t) B, 0.0f);
        for (auto& r : refBlock) r.assign ((size_t) B, 0.0f);
        outBlock.assign ((size_t) B, 0.0f);
        estBlock.assign ((size_t) B, 0.0f);
        forget();
    }

    // the signal state (2.0: what was learned - the path and the safety statistics - is kept; forget() clears it)
    void reset() noexcept
    {
        for (auto& x : X) std::fill (x.begin(), x.end(), std::complex<double> {});
        for (auto& b : xbuf) std::fill (b.begin(), b.end(), 0.0f);
        std::fill (PsiS.begin(), PsiS.end(), 0.0);
        std::fill (micRing.begin(), micRing.end(), 0.0f);
        std::fill (outBlock.begin(), outBlock.end(), 0.0f);
        std::fill (estBlock.begin(), estBlock.end(), 0.0f);
        psiInit = false;
        fill = 0;
        micPos = 0;
        newest = 0;
        constrainP = 0;
    }

    void forget() noexcept
    {
        for (auto& w : W) std::fill (w.begin(), w.end(), std::complex<double> {});
        for (auto& v : Pv) std::fill (v.begin(), v.end(), kInitVar);
        learned = 0;
        dot = 0.0;
        yy = 1.0e-30;
        weight = 0.0f;
        reset();
    }

    int latency() const noexcept { return B + lead; }

    // persistence: [ir: kRefs x P x B taps][uncertainty: kRefs x P][dot, yy]; same sample rate only (B, P depend on it)
    int pathSize() const noexcept { return kRefs * P * B + kRefs * P + 2; }
    int blockSize() const noexcept { return B; }
    int partitions() const noexcept { return P; }
    int64_t learnedBlocks() const noexcept { return learned; } // blocks adapted with the beat present

    void exportPath (float* dst) noexcept
    {
        float* ir = dst;
        float* unc = dst + kRefs * P * B;
        for (int r = 0; r < kRefs; ++r)
            for (int q = 0; q < P; ++q)
            {
                double pv = 0.0;
                for (int k = 0; k < K; ++k)
                {
                    tmpF[(size_t) (2 * k)] = (float) at (W[(size_t) r], q, k).real();
                    tmpF[(size_t) (2 * k + 1)] = (float) at (W[(size_t) r], q, k).imag();
                    pv += at (Pv[(size_t) r], q, k);
                }
                fft.inverse (tmpF.data(), tmpT.data());
                std::copy (tmpT.begin(), tmpT.begin() + B, ir + (r * P + q) * B);
                unc[r * P + q] = (float) (pv / K);
            }
        dst[pathSize() - 2] = (float) dot;
        dst[pathSize() - 1] = (float) yy;
        std::fill (tmpT.begin(), tmpT.end(), 0.0f);
    }

    // false (and nothing changed) if the data does not fit or is not finite
    bool importPath (const float* src, int n) noexcept
    {
        if (n != pathSize())
            return false;
        for (int i = 0; i < n; ++i)
            if (! std::isfinite (src[i]))
                return false;
        const float* unc = src + kRefs * P * B;
        for (int r = 0; r < kRefs; ++r)
            for (int q = 0; q < P; ++q)
            {
                std::copy (src + (r * P + q) * B, src + (r * P + q + 1) * B, tmpT.begin());
                std::fill (tmpT.begin() + B, tmpT.end(), 0.0f);
                forward (tmpT.data(), W[(size_t) r], q);
                const double pv = std::clamp ((double) unc[r * P + q], 1.0e-14, kInitVar);
                for (int k = 0; k < K; ++k)
                    at (Pv[(size_t) r], q, k) = pv;
            }
        std::fill (tmpT.begin(), tmpT.end(), 0.0f);
        dot = src[n - 2];
        yy = std::max (1.0e-30, (double) src[n - 1]);
        weight = forceWeight >= 0.0f ? forceWeight : (float) std::clamp (2.0 * dot / (yy + 1.0e-30), 0.0, 1.0);
        psiInit = false;
        learned = std::max<int64_t> (learned, 1);
        return true;
    }
    void setFrozen (bool f) noexcept { frozen = f; }
    float estimateWeight() const noexcept { return weight; }

    // one sample in (mono mic, reference L / R), one sample out: the mic `latency()` samples ago minus the estimated
    // bleed of that sample; est = the bleed estimate itself (for metering)
    float process (float mic, float refL, float refR, float& est) noexcept
    {
        const int R = (int) micRing.size();
        micRing[(size_t) micPos] = mic;
        micPos = (micPos + 1) % R;
        refBlock[0][(size_t) fill] = refL;
        refBlock[1][(size_t) fill] = refR;
        // the mic sample `lead` samples older belongs to this reference sample (the filter sees `lead` of the future)
        inBlock[(size_t) fill] = micRing[(size_t) ((micPos - 1 - lead + 2 * R) % R)];
        const float out = outBlock[(size_t) fill];
        est = estBlock[(size_t) fill];
        if (++fill == B)
        {
            fill = 0;
            block();
        }
        return out;
    }

private:
    template <typename T>
    T& at (std::vector<T>& v, int p, int k) noexcept { return v[(size_t) (p * K + k)]; }

    void forward (const float* t, std::vector<std::complex<double>>& dst, int p) noexcept
    {
        fft.forward (t, tmpF.data());
        for (int k = 0; k < K; ++k)
            at (dst, p, k) = { tmpF[(size_t) (2 * k)], tmpF[(size_t) (2 * k + 1)] };
    }

    void block() noexcept
    {
        // newest reference spectra into partition slot `newest` (a ring over partitions: slot newest = delay 0)
        newest = (newest - 1 + P) % P;
        for (int r = 0; r < kRefs; ++r)
        {
            std::copy (xbuf[(size_t) r].begin() + B, xbuf[(size_t) r].end(), xbuf[(size_t) r].begin());
            std::copy (refBlock[(size_t) r].begin(), refBlock[(size_t) r].end(), xbuf[(size_t) r].begin() + B);
            forward (xbuf[(size_t) r].data(), X[(size_t) r], newest);
        }
        // prediction: the path drifts (A), its uncertainty grows by the process noise
        const double A2 = kA * kA;
        for (int r = 0; r < kRefs; ++r)
            for (int i = 0; i < P * K; ++i)
            {
                W[(size_t) r][(size_t) i] *= kA;
                Pv[(size_t) r][(size_t) i] = A2 * Pv[(size_t) r][(size_t) i] + (1.0 - A2) * std::norm (W[(size_t) r][(size_t) i]) + 1.0e-12;
            }
        // echo estimate (overlap-save: the last B samples)
        std::fill (Y.begin(), Y.end(), std::complex<double> {});
        for (int r = 0; r < kRefs; ++r)
            for (int q = 0; q < P; ++q)
            {
                const int slot = (newest + q) % P; // delay q blocks
                for (int k = 0; k < K; ++k)
                    Y[(size_t) k] += at (W[(size_t) r], q, k) * at (X[(size_t) r], slot, k);
            }
        for (int k = 0; k < K; ++k)
        {
            tmpF[(size_t) (2 * k)] = (float) Y[(size_t) k].real();
            tmpF[(size_t) (2 * k + 1)] = (float) Y[(size_t) k].imag();
        }
        fft.inverse (tmpF.data(), tmpT.data());
        // error, output; the projection weight (safety)
        double d = 0.0, s = 0.0, mm = 0.0;
        for (int i = 0; i < B; ++i)
        {
            const float y = tmpT[(size_t) (B + i)];
            const float m = inBlock[(size_t) i];
            estBlock[(size_t) i] = y;
            d += (double) m * y;
            s += (double) y * y;
            mm += (double) m * m;
            tmpT[(size_t) i] = 0.0f;
            tmpT[(size_t) (B + i)] = m - y; // e (for the update)
        }
        // no mic signal (below -120 dBFS: the take has ended, the track is muted): nothing to learn from and nothing
        // to subtract - the beat goes on, the estimate would be subtracted from silence and the path unlearned
        if (mm < 1.0e-12 * B)
        {
            std::copy (inBlock.begin(), inBlock.end(), outBlock.begin());
            std::fill (estBlock.begin(), estBlock.end(), 0.0f);
            std::fill (tmpT.begin(), tmpT.end(), 0.0f);
            return;
        }
        // smoothed over ~0.5 s: the voice is uncorrelated with the beat and averages out, a wrong estimate does not
        // each block's evidence weighted by 1 / its mic energy: the pauses (where the bleed is plain) decide, loud
        // voice blocks (whose random correlation with the estimate is large) barely count. 2.0: by 1 / max(mic,
        // estimate) - a nearly silent mic under a loud estimate (a take fading out while the beat plays on) counts
        // as one pause, not as thousands
        const double wb = 1.0 / (std::max (mm, s) + 1.0e-12 * B);
        dot = wSmooth * dot + (1.0 - wSmooth) * d * wb;
        yy = wSmooth * yy + (1.0 - wSmooth) * s * wb;
        // subtracting the estimate lowers the mic's energy exactly when <mic, est> / <est, est> > 1/2: then cancel
        // fully; below that the estimate is (still) wrong and fades out (0 when it does not correlate at all)
        weight = forceWeight >= 0.0f ? forceWeight : (float) std::clamp (2.0 * dot / (yy + 1.0e-30), 0.0, 1.0);
        for (int i = 0; i < B; ++i)
        {
            outBlock[(size_t) i] = inBlock[(size_t) i] - weight * estBlock[(size_t) i];
            estBlock[(size_t) i] *= weight;
        }
        if (frozen)
            return;
        double rr = 0.0;
        for (int i = 0; i < B; ++i)
            rr += (double) refBlock[0][(size_t) i] * refBlock[0][(size_t) i] + (double) refBlock[1][(size_t) i] * refBlock[1][(size_t) i];
        if (rr > 1.0e-10 * B) // the beat is there (above -100 dBFS)
            ++learned;
        fft.forward (tmpT.data(), tmpF.data());
        for (int k = 0; k < K; ++k)
            E[(size_t) k] = { tmpF[(size_t) (2 * k)], tmpF[(size_t) (2 * k + 1)] };
        // measurement noise per bin: error power minus what the path uncertainty explains, floored, smoothed
        for (int k = 0; k < K; ++k)
        {
            double unc = 0.0;
            for (int r = 0; r < kRefs; ++r)
                for (int q = 0; q < P; ++q)
                    unc += std::norm (at (X[(size_t) r], (newest + q) % P, k)) * at (Pv[(size_t) r], q, k);
            const double e2 = std::norm (E[(size_t) k]);
            const double psi = std::max (e2 - unc, kPsiFloor * e2 + 1.0e-12);
            PsiS[(size_t) k] = psiInit ? 0.8 * PsiS[(size_t) k] + 0.2 * psi : psi;
            Den[(size_t) k] = unc + PsiS[(size_t) k] + 1.0e-20;
        }
        psiInit = true;
        // Kalman update; one partition per reference gets the gradient constraint (causal taps) this block
        for (int r = 0; r < kRefs; ++r)
            for (int q = 0; q < P; ++q)
            {
                const int slot = (newest + q) % P;
                for (int k = 0; k < K; ++k)
                {
                    const auto x = at (X[(size_t) r], slot, k);
                    const double pv = at (Pv[(size_t) r], q, k);
                    const auto kg = pv * std::conj (x) / Den[(size_t) k];
                    at (W[(size_t) r], q, k) += kg * E[(size_t) k];
                    at (Pv[(size_t) r], q, k) = std::max ((1.0 - 0.5 * std::real (kg * x)) * pv, 1.0e-14);
                }
                if (q == constrainP)
                {
                    for (int k = 0; k < K; ++k)
                    {
                        tmpF[(size_t) (2 * k)] = (float) at (W[(size_t) r], q, k).real();
                        tmpF[(size_t) (2 * k + 1)] = (float) at (W[(size_t) r], q, k).imag();
                    }
                    fft.inverse (tmpF.data(), tmpT.data());
                    std::fill (tmpT.begin() + B, tmpT.end(), 0.0f);
                    forward (tmpT.data(), W[(size_t) r], q);
                }
            }
        constrainP = (constrainP + 1) % P;
    }

    int B = 256, N = 512, K = 257, P = 12, lead = 384, fill = 0, micPos = 0, newest = 0, constrainP = 0;
    axell::RealFft fft;
    std::array<std::vector<std::complex<double>>, kRefs> W, X;
    std::array<std::vector<double>, kRefs> Pv;
    std::array<std::vector<float>, kRefs> xbuf, refBlock;
    std::vector<double> PsiS;
    std::array<double, 1025> Den {};
    std::vector<std::complex<double>> E, Y;
    std::vector<float> tmpT, tmpF, micRing, inBlock, outBlock, estBlock;
    bool psiInit = false, frozen = false;
    double dot = 0.0, yy = 1.0e-30;
    float weight = 0.0f;
    int64_t learned = 0;
public:
    float forceWeight = -1.0f; // research: fixed projection weight
    double wSmooth = 0.99;
private:
};
} // namespace axg
