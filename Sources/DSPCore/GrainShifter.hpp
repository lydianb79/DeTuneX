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
//     of the phasing/flanging of the plain two-head design, for chords too.
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
};

class GrainShifter {
public:
    void prepare() {
        buf_.assign(kBufSize, 0.f);
        corr_.assign(8192, 0.f);
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
        t_ = kBufSize; // start well past zero so "t - delay" is never negative
        latency_ = (float)(cfg_.searchRadius + kMargin);
    }

    // Takes effect at the next grain boundary (click-free). Audio thread only.
    void setConfig(const GrainConfig& c) { pending_ = c; }
    float latency() const { return latency_; }

    inline float process(float x, float ratio) {
        ++t_;
        buf_[t_ & kMask] = x;

        if (newest_ < 0 || !grains_[newest_].active || grains_[newest_].age * 2 >= grains_[newest_].len)
            spawn(ratio);

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
            const float w = win_[wi] + (win_[wi + 1] - win_[wi]) * wf;
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

    struct Grain {
        double pos = 0;
        int age = 0;
        int len = 1;
        float invLen = 1.f;
        bool active = false;
    };

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
        g.pos = (double)t_ - D;
        g.age = 0;
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
        const int C = cfg_.corrLen;
        range = std::min(range, (int)corr_.size() - 1);
        const int64_t refEnd = (int64_t)std::floor(refPos);
        float eRef = 0.f;
        for (int m = 1; m <= C; m += 2) { const float v = at(refEnd - m); eRef += v * v; }
        if (eRef < 1e-7f) return 0; // silence: shortest delay

        const int64_t candBase = t_ - (int64_t)std::ceil(dMin);
        float best = -2.f;
        int bestJ = 0;
        for (int j = 0; j <= range; ++j) {
            const int64_t ce = candBase - j;
            float dot = 0.f, e = 0.f;
            for (int m = 1; m <= C; m += 2) {
                const float c = at(ce - m);
                dot += c * at(refEnd - m);
                e += c * c;
            }
            const float v = dot / std::sqrt(e * eRef + 1e-12f);
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

    std::vector<float> buf_, win_, corr_;
    Grain grains_[kMaxGrains];
    int newest_ = -1;
    int64_t t_ = 0;
    GrainConfig cfg_, pending_;
    float latency_ = 0.f;
};

} // namespace dtx
