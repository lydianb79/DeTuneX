// GrainShifter.hpp — low-latency polyphonic time-domain pitch shifter.
//
// Algorithm: overlapping Hann-windowed read grains over a circular delay line
// (the classic "rotating tape head" shifter), improved in two ways:
//
//  1. Splice alignment. When a new grain starts, its read position is chosen
//     inside ±searchRadius of the nominal delay so that the waveform just
//     before it best matches the waveform just before the grain it crossfades
//     with (normalized cross-correlation over corrLen samples, looking
//     *backwards*, so the search costs no extra latency). This removes most
//     of the phasing/flanging of the plain two-head design. A long correlation
//     window (~1024 samples) matters a lot for chords: it makes the splice
//     match all strings at once instead of just the loudest partial.
//
//  2. Ratio-adaptive grains. Shifting down, a grain reads slower than real
//     time so the delay grows; we start each grain at the minimum safe delay.
//     Shifting up, a grain catches up with the write head, so the grain is
//     shortened for large up-shifts to keep the delay budget (and latency) small.
//
// Latency ≈ searchRadius + |1 - ratio| * grainLen / 2 (a few ms in "Tight").
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dtx {

struct GrainConfig {
    int grainLen = 1024;     // base grain length in samples
    int searchRadius = 300;  // ± samples searched for the best splice point
    int corrLen = 256;       // correlation window
    float latencyBias = 0.02f; // correlation tolerance when preferring a shorter delay
    // Transient snap: on a pick attack, cut straight to the newest audio (minimum
    // delay) with a short crossfade instead of waiting for an aligned splice.
    bool  snap = true;
    int   snapFade = 96;       // crossfade length in samples (2 ms @ 48 kHz)
    float snapRatio = 2.0f;    // fast/slow envelope ratio that counts as an attack
    float rateScale = 1.0f;    // sampleRate / 48000: keeps detector timing constant
};

class GrainShifter {
public:
    void prepare() {
        buf_.assign(kBufSize, 0.f);
        corr_.assign(8192, 0.f);
        ref_.assign(8192, 0.f);       // correlation window (corrLen) scratch
        chunk_.assign(16384, 0.f);    // candidate region (2 x searchRadius + corrLen) scratch
        prefix_.assign(16385, 0.0);
        win_.resize(kWinTable + 2);
        for (int i = 0; i <= kWinTable + 1; ++i) {
            const double s = std::sin(M_PI * std::min(i, kWinTable) / kWinTable);
            win_[i] = (float)(s * s);
        }
        reset();
    }

    void reset() {
        std::fill(buf_.begin(), buf_.end(), 0.f);
        for (auto& g : grains_) g = Grain{};
        newest_ = -1;
        envFast_ = envSlow_ = 0.f;
        holdoff_ = 0;
        t_ = kBufSize; // start well past zero so "t - delay" is never negative
        latency_ = (float)(cfg_.searchRadius + kMargin);
    }

    // Takes effect at the next grain boundary (click-free). Audio thread only.
    void setConfig(const GrainConfig& c) { pending_ = c; }
    float latency() const { return latency_; }

    inline float process(float x, float ratio) {
        ++t_;
        buf_[t_ & kMask] = x;

        if (cfg_.snap && detectAttack(x)) {
            snapTo(ratio);
        } else if (newest_ < 0 || !grains_[newest_].active || grains_[newest_].age * 2 >= grains_[newest_].len) {
            spawn(ratio);
        }

        const double maxPos = (double)(t_ - 2);
        const double minPos = (double)(t_ - (kBufSize - 8));
        float acc = 0.f, wsum = 0.f;
        for (auto& g : grains_) {
            if (!g.active) continue;
            if (g.pos > maxPos) g.pos = maxPos;      // ratio rose mid-grain: never read the future
            else if (g.pos < minPos) g.pos = minPos;
            const float ph = (float)g.age * g.invLen * kWinTable;
            const int wi = (int)ph;
            const float wf = ph - wi;
            float w = win_[wi] + (win_[wi + 1] - win_[wi]) * wf;
            if (g.fastRise > 0.f && g.age * 2 < g.len)                 // snapped grain: quick fade-in, then hold
                w = std::min(1.f, (float)g.age * g.fastRise);
            if (g.releaseAt >= 0) {                                    // cut short by a snap: quick fade-out
                const float k = 1.f - (float)(g.age - g.releaseAt) * g.releaseInv;
                if (k <= 0.f) { g.active = false; continue; }
                w *= k;
            }
            acc += w * read(g.pos);
            wsum += w;
            g.pos += ratio;
            if (++g.age >= g.len) g.active = false;
        }
        return acc / std::max(wsum, 0.5f);
    }

private:
    static constexpr int kBufBits = 15;
    static constexpr int kBufSize = 1 << kBufBits;
    static constexpr int64_t kMask = kBufSize - 1;
    static constexpr int kWinTable = 2048;
    static constexpr int kMaxGrains = 4;
    static constexpr int kMargin = 4;
    static constexpr int kSnapLead = 48;       // samples @48k before the detection point (1 ms)
    static constexpr int kSnapHoldoff = 4800;  // samples @48k: at most one snap per 100 ms

    struct Grain {
        double pos = 0;
        int age = 0;
        int len = 1;
        float invLen = 1.f;
        bool active = false;
        float fastRise = 0.f;   // >0: snapped grain, 1 / fade length
        int releaseAt = -1;     // age at which a snap started fading this grain out
        float releaseInv = 0.f;
    };

    // Attack detector on the input: fast envelope jumps well above the slow one.
    inline bool detectAttack(float x) {
        const float a = std::fabs(x);
        // Both are peak envelopes, so a chord's crest factor doesn't look like an attack.
        const float k = 1.f / std::max(cfg_.rateScale, 0.25f);
        envFast_ += (a - envFast_) * (a > envFast_ ? 0.25f * k : 0.0025f * k);   // ~0.1 ms attack, ~8 ms release
        envSlow_ += (envFast_ - envSlow_) * 0.0006f * k;                         // ~35 ms smoothing of the peak envelope
        if (holdoff_ > 0) { --holdoff_; return false; }
        if (envFast_ > 0.004f && envFast_ > cfg_.snapRatio * envSlow_ + 0.002f) {
            holdoff_ = (int)(kSnapHoldoff * cfg_.rateScale);
            return true;
        }
        return false;
    }

    void snapTo(float ratioF) {
        cfg_ = pending_;
        const int fade = std::max(cfg_.snapFade, 8);
        for (auto& g : grains_)
            if (g.active && g.releaseAt < 0) { g.releaseAt = g.age; g.releaseInv = 1.f / (float)fade; }
        const double r = std::clamp((double)ratioF, 0.1, 8.0);
        double len = cfg_.grainLen, upDelay = 0.0;
        if (r > 1.0) {
            const double budget = 0.5 * cfg_.grainLen * std::max(1.0, std::sqrt(r - 1.0));
            len = std::max(std::min((double)cfg_.grainLen, budget / (r - 1.0)), 96.0);
            upDelay = (r - 1.0) * len;
        }
        // Start a little before the detection point so the very start of the pick
        // attack is included (the detector fires ~0.5 ms into the attack).
        const double D = std::ceil(kMargin + upDelay) + std::ceil(kSnapLead * cfg_.rateScale);
        int slot = -1;
        for (int i = 0; i < kMaxGrains; ++i) if (!grains_[i].active) { slot = i; break; }
        if (slot < 0) {   // all busy: replace the one that is fading out furthest
            slot = 0;
            for (int i = 1; i < kMaxGrains; ++i) if (grains_[i].age > grains_[slot].age) slot = i;
        }
        Grain& g = grains_[slot];
        g = Grain{};
        g.pos = (double)t_ - D;
        g.len = std::max((int)len, 2);
        g.invLen = 1.f / (float)g.len;
        g.active = true;
        g.fastRise = 1.f / (float)fade;
        newest_ = slot;
        const float avg = (float)(D + (1.0 - r) * g.len * 0.5);
        latency_ += 0.25f * (avg - latency_);
    }

    inline float at(int64_t i) const { return buf_[i & kMask]; }

    // 4-point, 3rd-order Hermite interpolation.
    inline float read(double pos) const {
        const int64_t i = (int64_t)std::floor(pos);
        const float f = (float)(pos - (double)i);
        const float xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }

    void spawn(float ratioF) {
        cfg_ = pending_;
        const double r = std::clamp((double)ratioF, 0.1, 8.0);
        const int S = cfg_.searchRadius;

        double len = cfg_.grainLen;
        double upDelay = 0.0;
        if (r > 1.0) {
            const double budget = 0.5 * cfg_.grainLen * std::max(1.0, std::sqrt(r - 1.0));
            len = std::min((double)cfg_.grainLen, budget / (r - 1.0));
            len = std::max(len, 96.0);
            upDelay = (r - 1.0) * len;
        }
        const int L = (int)len;
        // Candidate delays: [dMin, dMin + 2S]. dMin is the smallest delay that never
        // overtakes the write head during this grain.
        const double dMin = kMargin + upDelay;

        double extra = S; // no history to align with: sit in the middle of the range
        if (S > 0 && newest_ >= 0 && grains_[newest_].active) {
            const double refPos = grains_[newest_].pos;
            // The search runs on integer lags relative to floor(refPos); carry the
            // fractional read position over so splices are sample-accurate.
            extra = searchSplice(refPos, dMin, 2 * S) - (refPos - std::floor(refPos));
        } else if (S == 0) {
            extra = 0;
        }

        const double D = std::ceil(dMin) + extra;

        int slot = 0;
        for (int i = 0; i < kMaxGrains; ++i) if (!grains_[i].active) { slot = i; break; }
        Grain& g = grains_[slot];
        g = Grain{};
        g.pos = (double)t_ - D;
        g.len = std::max(L, 2);
        g.invLen = 1.f / (float)g.len;
        g.active = true;
        newest_ = slot;

        const float avg = (float)(D + (1.0 - r) * L * 0.5);
        latency_ += 0.25f * (avg - latency_);
    }

    // Returns j in [0, range]: the extra delay (beyond dMin) whose preceding
    // corrLen samples best match those preceding refPos (normalized
    // cross-correlation). Among correlation peaks that are within
    // `latencyBias` of the best one, the shortest delay wins, so latency stays
    // as low as the signal allows without ever accepting a misaligned splice.
    double searchSplice(double refPos, double dMin, int range) {
        const int C = std::min(cfg_.corrLen, (int)ref_.size());
        range = std::min({range, (int)corr_.size() - 1, (int)chunk_.size() - C});
        const int64_t refEnd = (int64_t)std::floor(refPos);
        const int64_t candBase = t_ - (int64_t)std::ceil(dMin);

        // Copy the reference window and the whole candidate region into contiguous
        // scratch memory once, so the inner products below are plain, vectorisable loops.
        float* ref = ref_.data();
        float* chunk = chunk_.data();
        for (int m = 0; m < C; ++m) ref[m] = at(refEnd - C + m);
        const int64_t chunkStart = candBase - range - C;      // oldest sample any candidate uses
        const int chunkLen = range + C;
        for (int i = 0; i < chunkLen; ++i) chunk[i] = at(chunkStart + i);

        float eRef = 0.f;
        for (int m = 0; m < C; ++m) eRef += ref[m] * ref[m];
        if (eRef < 1e-7f) return 0; // silence: shortest delay

        // Window energies from a prefix sum (double: no cancellation drift).
        double* pre = prefix_.data();
        pre[0] = 0.0;
        for (int i = 0; i < chunkLen; ++i) pre[i + 1] = pre[i] + (double)chunk[i] * chunk[i];

        float best = -2.f;
        int bestJ = 0;
        for (int j = 0; j <= range; ++j) {
            // candidate j ends at candBase - j  →  chunk offset (range - j)
            const float* c = chunk + (range - j);
            float d0 = 0.f, d1 = 0.f, d2 = 0.f, d3 = 0.f;
            int m = 0;
            for (; m + 3 < C; m += 4) {
                d0 += c[m] * ref[m];         d1 += c[m + 1] * ref[m + 1];
                d2 += c[m + 2] * ref[m + 2]; d3 += c[m + 3] * ref[m + 3];
            }
            for (; m < C; ++m) d0 += c[m] * ref[m];
            const double e = pre[range - j + C] - pre[range - j];
            const float v = (float)((d0 + d1 + d2 + d3) / std::sqrt(e * eRef + 1e-12));
            corr_[j] = v;
            if (v > best) { best = v; bestJ = j; }
        }
        const float accept = best - cfg_.latencyBias;
        for (int j = 1; j < range; ++j) {
            const float v = corr_[j];
            if (v >= accept && v >= corr_[j - 1] && v >= corr_[j + 1]) return refine(j); // earliest good peak
        }
        return (bestJ > 0 && bestJ < range) ? refine(bestJ) : (double)bestJ;
    }

    // Parabolic interpolation of the correlation peak: sub-sample splice
    // positions keep small shifts (detune) exactly in tune.
    double refine(int j) const {
        const float a = corr_[j - 1], b = corr_[j], c = corr_[j + 1];
        const float den = a - 2.f * b + c;
        if (std::fabs(den) < 1e-9f) return j;
        return j + std::clamp(0.5f * (a - c) / den, -0.5f, 0.5f);
    }

    std::vector<float> buf_, win_, corr_, ref_, chunk_;
    std::vector<double> prefix_;
    Grain grains_[kMaxGrains];
    int newest_ = -1;
    int64_t t_ = 0;
    float envFast_ = 0.f, envSlow_ = 0.f;
    int holdoff_ = 0;
    GrainConfig cfg_, pending_;
    float latency_ = 0.f;
};

} // namespace dtx
