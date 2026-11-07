// FFT.hpp — small in-place iterative radix-2 complex FFT (no allocation after init).
#pragma once
#include <cmath>
#include <vector>

namespace dtx {

class FFT {
public:
    void init(int n) {
        n_ = n;
        bits_ = 0;
        while ((1 << bits_) < n) ++bits_;
        rev_.assign(n, 0);
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits_; ++b) if (i & (1 << b)) r |= 1 << (bits_ - 1 - b);
            rev_[i] = r;
        }
        cos_.resize(n / 2);
        sin_.resize(n / 2);
        for (int i = 0; i < n / 2; ++i) {
            const double a = -2.0 * M_PI * i / n;
            cos_[i] = (float)std::cos(a);
            sin_[i] = (float)std::sin(a);
        }
    }

    int size() const { return n_; }

    // inverse = true computes the unscaled inverse transform.
    void transform(float* re, float* im, bool inverse) const {
        const int n = n_;
        for (int i = 0; i < n; ++i) {
            const int j = rev_[i];
            if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        const float sgn = inverse ? -1.f : 1.f;
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len >> 1;
            const int step = n / len;
            for (int i = 0; i < n; i += len) {
                for (int k = 0; k < half; ++k) {
                    const float wr = cos_[k * step];
                    const float wi = sgn * sin_[k * step];
                    const int a = i + k, b = a + half;
                    const float tr = re[b] * wr - im[b] * wi;
                    const float ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr;        im[a] += ti;
                }
            }
        }
    }

private:
    int n_ = 0, bits_ = 0;
    std::vector<int> rev_;
    std::vector<float> cos_, sin_;
};

} // namespace dtx
