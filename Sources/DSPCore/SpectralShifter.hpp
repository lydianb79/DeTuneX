// SpectralShifter.hpp — phase-locked phase-vocoder pitch shifter ("Studio" engine).
//
// Peak-based frequency-domain shifting after Laroche & Dolson (1999):
// spectral peaks are located, each peak's region of influence is moved as a
// block to the bin nearest ratio * true_frequency, and all bins in a region
// receive the same phase rotation as their peak (identity phase locking), so
// the vertical phase coherence of each partial is preserved. Highest
// polyphonic fidelity for large intervals, at the cost of one FFT frame of
// latency (N = 2048 @ 48 kHz ≈ 43 ms) — for studio/practice, not tight playing.
#pragma once
#include "FFT.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace dtx {

class SpectralShifter {
public:
    void prepare(double sampleRate) {
        N_ = sampleRate > 70000.0 ? 4096 : 2048;   // ~43 ms: resolves guitar chords
        hop_ = N_ / 4;
        half_ = N_ / 2;
        fft_.init(N_);
        win_.resize(N_);
        for (int i = 0; i < N_; ++i) win_[i] = (float)(0.5 - 0.5 * std::cos(2.0 * M_PI * i / N_));
        // Hann analysis * Hann synthesis at 75 % overlap sums to 1.5.
        outScale_ = 1.f / (1.5f * (float)N_);
        const int nb = half_ + 1;
        in_.assign(N_, 0.f);   out_.assign(N_, 0.f);
        re_.assign(N_, 0.f);   im_.assign(N_, 0.f);
        mag_.assign(nb, 0.f);  ph_.assign(nb, 0.f);
        prevPh_.assign(nb, 0.f);
        synth_.assign(nb, 0.f); newSynth_.assign(nb, 0.f);
        oRe_.assign(nb, 0.f);  oIm_.assign(nb, 0.f);
        written_.assign(nb, 0);
        peaks_.reserve(nb); lo_.reserve(nb); hi_.reserve(nb);
        reset();
    }

    void reset() {
        std::fill(in_.begin(), in_.end(), 0.f);
        std::fill(out_.begin(), out_.end(), 0.f);
        std::fill(prevPh_.begin(), prevPh_.end(), 0.f);
        std::fill(synth_.begin(), synth_.end(), 0.f);
        inPos_ = outPos_ = hopCount_ = 0;
    }

    int latency() const { return N_; }

    inline float process(float x, float ratio) {
        in_[inPos_] = x;
        if (++inPos_ == N_) inPos_ = 0;
        if (++hopCount_ == hop_) { hopCount_ = 0; frame(ratio); }
        const float y = out_[outPos_];
        out_[outPos_] = 0.f;
        if (++outPos_ == N_) outPos_ = 0;
        return y;
    }

private:
    static inline float princarg(float a) {
        return a - 2.f * (float)M_PI * std::floor((a + (float)M_PI) / (2.f * (float)M_PI));
    }

    void frame(float ratio) {
        const int N = N_, H = half_;
        for (int n = 0; n < N; ++n) {
            int idx = inPos_ + n; if (idx >= N) idx -= N;   // inPos_ is the oldest sample
            re_[n] = in_[idx] * win_[n];
            im_[n] = 0.f;
        }
        fft_.transform(re_.data(), im_.data(), false);

        float maxMag = 0.f;
        for (int k = 0; k <= H; ++k) {
            mag_[k] = std::sqrt(re_[k] * re_[k] + im_[k] * im_[k]);
            ph_[k] = std::atan2(im_[k], re_[k]);
            maxMag = std::max(maxMag, mag_[k]);
        }

        // Peak picking (local maxima above -70 dB of frame max).
        peaks_.clear();
        const float thresh = std::max(maxMag * 3.2e-4f, 1e-6f);
        for (int k = 1; k < H; ++k) {
            const float m = mag_[k];
            if (m < thresh || m < mag_[k - 1] || m <= mag_[k + 1]) continue;
            peaks_.push_back(k);
        }

        // Regions of influence: split at the magnitude minimum between peaks.
        lo_.clear(); hi_.clear();
        const int P = (int)peaks_.size();
        for (int i = 0; i < P; ++i) {
            int lo = 0, hi = H;
            if (i > 0) {
                int a = peaks_[i - 1], b = peaks_[i], mn = a + 1;
                for (int k = a + 1; k < b; ++k) if (mag_[k] < mag_[mn]) mn = k;
                lo = mn;
            }
            if (i + 1 < P) {
                int a = peaks_[i], b = peaks_[i + 1], mn = a + 1;
                for (int k = a + 1; k < b; ++k) if (mag_[k] < mag_[mn]) mn = k;
                hi = mn - 1;
            }
            lo_.push_back(lo); hi_.push_back(std::max(hi, lo));
        }

        const float twoPiOverN = 2.f * (float)M_PI / (float)N;
        std::fill(oRe_.begin(), oRe_.end(), 0.f);
        std::fill(oIm_.begin(), oIm_.end(), 0.f);
        std::fill(written_.begin(), written_.end(), 0);

        for (int i = 0; i < P; ++i) {
            const int p = peaks_[i];
            const float expected = twoPiOverN * p * hop_;
            const float dphi = princarg(ph_[p] - prevPh_[p] - expected);
            const float omega = twoPiOverN * p + dphi / (float)hop_; // rad/sample
            const float newOmega = omega * ratio;
            const int q = (int)std::lround(newOmega / twoPiOverN);
            if (q < 1 || q >= H) continue;
            const int shift = q - p;
            const float psi = synth_[q] + newOmega * (float)hop_;
            const float theta = psi - ph_[p];
            if (ratio >= 1.f) {
                // Up: move the region as a block (regions spread apart, no collisions).
                for (int k = lo_[i]; k <= hi_[i]; ++k) {
                    const int kk = k + shift;
                    if (kk < 0 || kk > H) continue;
                    put(kk, mag_[k], ph_[k] + theta);
                }
            } else {
                // Down: partials get closer together, so compress each region by the
                // ratio around its new centre; otherwise neighbouring regions overlap
                // and cancel (audible as missing chord tones on octave-down).
                const int a = std::max(0, (int)std::ceil(q + (lo_[i] - p) * ratio));
                const int b = std::min(H, (int)std::floor(q + (hi_[i] - p) * ratio));
                const float inv = 1.f / ratio;
                // Keep the partial's amplitude: the summed magnitude of the
                // compressed region must equal that of the original region.
                float sumIn = 0.f, sumOut = 0.f;
                for (int k = lo_[i]; k <= hi_[i]; ++k) sumIn += mag_[k];
                for (int kk = a; kk <= b; ++kk)
                    sumOut += mag_[std::clamp(p + (int)std::lround((kk - q) * inv), lo_[i], hi_[i])];
                // (exponent 0.8: empirically flat level from -24 to 0 semitones)
                const float g = sumOut > 1e-12f ? std::pow(sumIn / sumOut, 0.8f) : 1.f;
                for (int kk = a; kk <= b; ++kk) {
                    const int k = std::clamp(p + (int)std::lround((kk - q) * inv), lo_[i], hi_[i]);
                    put(kk, g * mag_[k], ph_[k] + theta);
                }
            }
        }
        for (int k = 0; k <= H; ++k) {
            if (!written_[k]) newSynth_[k] = princarg(synth_[k] + twoPiOverN * k * hop_ * ratio);
            synth_[k] = newSynth_[k];
            prevPh_[k] = ph_[k];
        }

        // Hermitian spectrum → real signal.
        re_[0] = oRe_[0]; im_[0] = 0.f;
        re_[H] = oRe_[H]; im_[H] = 0.f;
        for (int k = 1; k < H; ++k) {
            re_[k] = oRe_[k];      im_[k] = oIm_[k];
            re_[N - k] = oRe_[k];  im_[N - k] = -oIm_[k];
        }
        fft_.transform(re_.data(), im_.data(), true);

        int o = outPos_;
        for (int n = 0; n < N; ++n) {
            out_[o] += re_[n] * win_[n] * outScale_;
            if (++o == N) o = 0;
        }
    }

    inline void put(int kk, float m, float phase) {
        oRe_[kk] += m * std::cos(phase);
        oIm_[kk] += m * std::sin(phase);
        newSynth_[kk] = princarg(phase);
        written_[kk] = 1;
    }

    FFT fft_;
    int N_ = 1024, hop_ = 256, half_ = 512;
    float outScale_ = 1.f;
    std::vector<float> win_, in_, out_, re_, im_, mag_, ph_, prevPh_, synth_, newSynth_, oRe_, oIm_;
    std::vector<char> written_;
    std::vector<int> peaks_, lo_, hi_;
    int inPos_ = 0, outPos_ = 0, hopCount_ = 0;
};

} // namespace dtx
