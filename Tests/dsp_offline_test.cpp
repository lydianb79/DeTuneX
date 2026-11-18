// Offline verification of the DSP core (builds on macOS or Linux):
//   make test
// Checks pitch accuracy (single notes and a chord), level stability,
// latency, glide behaviour and realtime CPU cost for every engine.
#include "DSPCore.h"
#include "../Sources/DSPCore/FFT.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>

static const double SR = 48000.0;
static int failures = 0;

static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static std::vector<float> render(DTXProcessor* p, const std::vector<float>& in, int block = 64) {
    std::vector<float> out(in.size());
    for (size_t i = 0; i < in.size(); i += block) {
        int n = (int)std::min<size_t>(block, in.size() - i);
        dtx_processor_process(p, in.data() + i, out.data() + i, n);
    }
    return out;
}

// Spectrum peak list of a segment, parabolic-interpolated, in Hz.
static std::vector<std::pair<double, double>> peaks(const std::vector<float>& x, size_t start, int topN) {
    const int N = 32768;
    dtx::FFT fft; fft.init(N);
    std::vector<float> re(N), im(N, 0.f);
    for (int i = 0; i < N; ++i) {
        double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / N);
        re[i] = (float)(x[start + i] * w);
    }
    fft.transform(re.data(), im.data(), false);
    std::vector<double> mag(N / 2);
    for (int k = 0; k < N / 2; ++k) mag[k] = std::log(std::sqrt((double)re[k] * re[k] + (double)im[k] * im[k]) + 1e-12);
    std::vector<std::pair<double, double>> pk;
    for (int k = 3; k < N / 2 - 3; ++k)
        if (mag[k] > mag[k - 1] && mag[k] >= mag[k + 1] && mag[k] > mag[k - 2] && mag[k] >= mag[k + 2]) {
            double a = mag[k - 1], b = mag[k], c = mag[k + 1];
            double d = 0.5 * (a - c) / (a - 2 * b + c);
            pk.push_back({b, (k + d) * SR / N});
        }
    std::sort(pk.begin(), pk.end(), [](auto& l, auto& r) { return l.first > r.first; });
    if ((int)pk.size() > topN) pk.resize(topN);
    return pk;
}

static double cents(double f, double ref) { return 1200.0 * std::log2(f / ref); }

static std::vector<float> tones(const std::vector<double>& freqs, int len, double amp = 0.25) {
    std::vector<float> v(len);
    for (int i = 0; i < len; ++i) {
        double s = 0;
        for (size_t j = 0; j < freqs.size(); ++j) s += std::sin(2 * M_PI * freqs[j] * i / SR + j);
        v[i] = (float)(amp * s);
    }
    return v;
}

static DTXProcessor* makeProc(int engine, float semis) {
    DTXProcessor* p = dtx_processor_create();
    dtx_processor_prepare(p, SR, 512);
    dtx_set_param(p, DTX_PARAM_ENGINE, (float)engine);
    dtx_set_param(p, DTX_PARAM_SEMITONES, semis);
    dtx_set_param(p, DTX_PARAM_BALANCE, 1.f);
    dtx_set_param(p, DTX_PARAM_ENGAGED, 1.f);
    return p;
}

static const char* engineName(int e) {
    static const char* n[] = {"Tight", "Balanced", "Wide", "Studio"};
    return n[e];
}

static void testPitch(int engine, double f0, float semis, double tolCents) {
    DTXProcessor* p = makeProc(engine, semis);
    auto in = tones({f0}, (int)SR * 2);
    auto out = render(p, in);
    auto pk = peaks(out, (size_t)SR / 2, 1);
    double expect = f0 * std::pow(2.0, semis / 12.0);
    double err = pk.empty() ? 9999 : cents(pk[0].second, expect);
    char msg[256];
    std::snprintf(msg, sizeof msg, "%-8s %6.1f Hz %+6.2f st -> %8.2f Hz (expect %8.2f, %+6.1f cents)",
                  engineName(engine), f0, semis, pk.empty() ? 0 : pk[0].second, expect, err);
    check(std::fabs(err) < tolCents, msg);
    dtx_processor_destroy(p);
}

// Fraction of output power within ±20 cents (or ±3 FFT bins) of the expected shifted partials.
static double concentration(const std::vector<float>& x, size_t start, const std::vector<double>& expect) {
    const int N = 32768;
    dtx::FFT fft; fft.init(N);
    std::vector<float> re(N), im(N, 0.f);
    for (int i = 0; i < N; ++i) re[i] = (float)(x[start + i] * (0.5 - 0.5 * std::cos(2 * M_PI * i / N)));
    fft.transform(re.data(), im.data(), false);
    double tot = 0, in = 0;
    for (int k = 4; k < N / 2; ++k) {
        double pw = (double)re[k] * re[k] + (double)im[k] * im[k], f = k * SR / N;
        tot += pw;
        for (double e : expect) if (std::fabs(cents(f, e)) < 20 || std::fabs(f - e) < 3 * SR / N) { in += pw; break; }
    }
    return in / tot;
}

// Guitar-like chord: 4 harmonics per string, equal-tempered (not exactly periodic).
static void testChord(int engine, const char* name, std::vector<double> roots, float semis, double minConc, bool infoOnly = false) {
    std::vector<float> in((size_t)SR * 2, 0.f);
    std::vector<double> expect;
    const double r = std::pow(2.0, semis / 12.0);
    for (size_t j = 0; j < roots.size(); ++j)
        for (int h = 1; h <= 4; ++h) {
            expect.push_back(roots[j] * h * r);
            for (size_t i = 0; i < in.size(); ++i) in[i] += (float)(0.1 / h * std::sin(2 * M_PI * roots[j] * h * i / SR + j * 1.7 + h));
        }
    DTXProcessor* p = makeProc(engine, semis);
    auto out = render(p, in);
    double c = concentration(out, (size_t)SR / 2, expect);
    char msg[160];
    std::snprintf(msg, sizeof msg, "%-8s %-7s %+6.1f st: %5.1f %% of energy on target partials", engineName(engine), name, semis, 100 * c);
    if (infoOnly) std::printf("  [INFO] %s  (known limitation of this engine)\n", msg);
    else check(c >= minConc, msg);
    dtx_processor_destroy(p);
}

// Single sine; RMS measured over windows of exactly 4 output periods, so any
// ripple is amplitude modulation introduced by the shifter.
static void testLevel(int engine, float semis) {
    DTXProcessor* p = makeProc(engine, semis);
    const double f0 = 220.0, fo = f0 * std::pow(2.0, semis / 12.0);
    auto out = render(p, tones({f0}, (int)SR * 2, 0.3));
    const int W = (int)std::lround(4 * SR / fo);
    double lo = 1e9, hi = 0, sum = 0; int n = 0;
    for (size_t s = SR / 2; s + W < out.size(); s += W / 2) {
        double rr = 0; for (int i = 0; i < W; ++i) rr += out[s + i] * out[s + i];
        rr = std::sqrt(rr / W); lo = std::min(lo, rr); hi = std::max(hi, rr); sum += rr; ++n;
    }
    const double inRms = 0.3 / std::sqrt(2.0);
    const double ripple = 20 * std::log10(hi / lo), gain = 20 * std::log10(sum / n / inRms);
    char msg[160];
    std::snprintf(msg, sizeof msg, "%-8s level %+6.1f st: gain %+5.2f dB, ripple %4.2f dB", engineName(engine), semis, gain, ripple);
    check(ripple < 3.0 && std::fabs(gain) < 1.5, msg);
    dtx_processor_destroy(p);
}

static void testLatency(int engine, float semis) {
    DTXProcessor* p = makeProc(engine, semis);
    // Silence then a tone burst; latency = first output above 10 % of steady level.
    std::vector<float> in((size_t)SR, 0.f);
    const int onset = 24000;
    for (size_t i = onset; i < in.size(); ++i) in[i] = 0.3f * (float)std::sin(2 * M_PI * 220 * (i - onset) / SR);
    auto out = render(p, in);
    int first = -1;
    for (size_t i = onset; i < out.size(); ++i) if (std::fabs(out[i]) > 0.03f) { first = (int)(i - onset); break; }
    float reported = dtx_get_latency_samples(p);
    char msg[160];
    std::snprintf(msg, sizeof msg, "%-8s latency %+5.1f st: onset after %5.2f ms (reported avg %5.2f ms)",
                  engineName(engine), semis, first * 1000.0 / SR, reported * 1000.0 / SR);
    check(first >= 0 && first * 1000.0 / SR < (engine == 3 ? 50.0 : 20.0), msg);
    dtx_processor_destroy(p);
}

static void testGlide() {
    DTXProcessor* p = makeProc(1, 12.f);
    dtx_set_param(p, DTX_PARAM_ENGAGED, 0.f);
    dtx_set_param(p, DTX_PARAM_RISE_MS, 500.f);
    dtx_set_param(p, DTX_PARAM_FALL_MS, 250.f);
    std::vector<float> buf(4800, 0.f);
    dtx_processor_process(p, buf.data(), buf.data(), 4800);
    dtx_set_param(p, DTX_PARAM_ENGAGED, 1.f);
    for (int i = 0; i < 5; ++i) dtx_processor_process(p, buf.data(), buf.data(), 2400); // 250 ms
    float mid = dtx_get_current_semitones(p);
    for (int i = 0; i < 10; ++i) dtx_processor_process(p, buf.data(), buf.data(), 2400);
    float top = dtx_get_current_semitones(p);
    dtx_set_param(p, DTX_PARAM_ENGAGED, 0.f);
    for (int i = 0; i < 10; ++i) dtx_processor_process(p, buf.data(), buf.data(), 2400);
    float end = dtx_get_current_semitones(p);
    float wet = dtx_get_wet_amount(p);
    char msg[160];
    std::snprintf(msg, sizeof msg, "glide: mid-rise %.2f st, full %.2f st, after fall %.2f st, wet %.2f", mid, top, end, wet);
    check(mid > 4.5f && mid < 7.5f && top > 11.9f && std::fabs(end) < 0.05f && wet < 0.01f, msg);

    // Expression pedal scales linearly.
    dtx_set_param(p, DTX_PARAM_RISE_MS, 0.f);
    dtx_set_param(p, DTX_PARAM_ENGAGED, 1.f);
    dtx_set_param(p, DTX_PARAM_EXPRESSION_ACTIVE, 1.f);
    dtx_set_param(p, DTX_PARAM_EXPRESSION, 0.25f);
    for (int i = 0; i < 10; ++i) dtx_processor_process(p, buf.data(), buf.data(), 2400);
    float ex = dtx_get_current_semitones(p);
    std::snprintf(msg, sizeof msg, "expression 25%% of +12 st -> %.2f st", ex);
    check(std::fabs(ex - 3.f) < 0.05f, msg);
    dtx_processor_destroy(p);
}

static void testBypass() {
    DTXProcessor* p = makeProc(1, -12.f);
    dtx_set_param(p, DTX_PARAM_ENGAGED, 0.f);
    dtx_processor_reset(p);
    auto in = tones({330}, 9600);
    auto out = render(p, in);
    double maxErr = 0;
    for (size_t i = 0; i < in.size(); ++i) maxErr = std::max(maxErr, (double)std::fabs(out[i] - in[i]));
    char msg[120];
    std::snprintf(msg, sizeof msg, "bypass is bit-transparent at unity gain (max err %.2e)", maxErr);
    check(maxErr < 1e-6, msg);
    dtx_processor_destroy(p);
}

static void testCpu() {
    for (int e = 0; e < 4; ++e) {
        DTXProcessor* p = makeProc(e, -5.f);
        auto in = tones({110, 165, 220}, (int)SR * 10, 0.1);
        std::vector<float> out(in.size());
        auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < in.size(); i += 32) dtx_processor_process(p, in.data() + i, out.data() + i, 32);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        char msg[120];
        std::snprintf(msg, sizeof msg, "%-8s CPU: %.2f %% of one core (10 s audio in %.0f ms, 32-frame blocks)",
                      engineName(e), 100.0 * sec / 10.0, sec * 1000);
        check(sec / 10.0 < 0.25, msg);
        dtx_processor_destroy(p);
    }
}

// ---------------------------------------------------------------------------
// Realistic strummed chords: additive string model with inharmonic partials,
// per-partial decay, a few cents of mistuning per string and an 18 ms strum.
// Score = energy that is NOT on any correctly shifted partial (warble sidebands,
// smearing, ghost tones), relative to total, in dB. A perfect shifter scores
// about -42 dB on this set; lower is better.
// ---------------------------------------------------------------------------
struct StrNote { double f, t0, amp; };

static std::vector<float> strum(const std::vector<StrNote>& notes, double seconds, double ratio) {
    std::vector<float> y((size_t)(seconds * SR), 0.f);
    for (size_t n = 0; n < notes.size(); ++n) {
        const auto& nt = notes[n];
        const size_t s0 = (size_t)(nt.t0 * SR);
        for (int h = 1; h <= 18; ++h) {
            const double fh = nt.f * h * std::sqrt(1 + 0.00012 * h * h) * ratio;
            if (fh > 0.45 * SR) break;
            const double a = nt.amp / std::pow(h, 0.9) * std::fabs(0.6 + 0.4 * std::sin(h * 1.3));
            const double tau = 2.5 / (1.0 + 0.15 * h * h * (nt.f / 110.0));
            const double w = 2 * M_PI * fh / SR, ph = 0.7 * h + 1.9 * n;
            for (size_t i = s0; i < y.size(); ++i) {
                const double t = (i - s0) / SR, env = std::exp(-t / tau) * std::min(1.0, t / 0.002);
                if (env < 1e-4 && t > 0.01) break;
                y[i] += (float)(a * env * std::sin(w * (i - s0) + ph));
            }
        }
    }
    float pk = 0; for (float v : y) pk = std::max(pk, std::fabs(v));
    for (float& v : y) v *= 0.5f / pk;
    return y;
}

static double artifactDb(const std::vector<float>& x, std::vector<double> targets) {
    std::sort(targets.begin(), targets.end());
    const int N = 16384, H = 4096;
    dtx::FFT fft; fft.init(N);
    std::vector<float> re(N), im(N);
    double tot = 0, off = 0;
    for (size_t s = (size_t)(0.15 * SR); s + N <= x.size(); s += H) {
        for (int i = 0; i < N; ++i) { re[i] = (float)(x[s + i] * (0.5 - 0.5 * std::cos(2 * M_PI * i / N))); im[i] = 0; }
        fft.transform(re.data(), im.data(), false);
        for (int k = 4; k < N / 2; ++k) {
            const double f = k * SR / N;
            if (f > 12000) break;
            const double p = (double)re[k] * re[k] + (double)im[k] * im[k];
            tot += p;
            auto it = std::lower_bound(targets.begin(), targets.end(), f);
            double d = 1e9;
            if (it != targets.end()) d = std::min(d, std::fabs(*it - f));
            if (it != targets.begin()) d = std::min(d, std::fabs(*(it - 1) - f));
            if (d > std::max(3.0 * SR / N, f * 0.0087)) off += p;
        }
    }
    return 10 * std::log10(off / tot + 1e-12);
}

static void testStrummedChords(int engine, double maxMeanDb) {
    const std::vector<std::vector<double>> shapes = {
        {82.41, 123.47, 164.81, 207.65, 246.94, 329.63},   // E
        {98.00, 123.47, 146.83, 196.00, 246.94, 392.00},   // G
        {130.81, 164.81, 196.00, 261.63, 329.63},          // C
        {110.00, 164.81, 220.00, 261.63, 329.63},          // Am
        {146.83, 220.00, 293.66, 369.99},                  // D
        {82.41, 123.47, 164.81},                           // E5
        {392.00, 493.88, 587.33}};                         // high triad
    const float shifts[] = {-12, -5, -2, -1, 0.15f, 2, 5, 7, 12};
    double total = 0; int count = 0; double worst = -1e9; float worstShift = 0;
    for (float semis : shifts) {
        double sum = 0;
        for (size_t c = 0; c < shapes.size(); ++c) {
            std::vector<StrNote> notes;
            for (size_t i = 0; i < shapes[c].size(); ++i) {
                const double detune = std::pow(2.0, (3.0 * std::sin(7.1 * i + c)) / 1200.0); // ±3 cents
                notes.push_back({shapes[c][i] * detune, 0.1 + 0.018 * i, 1.0 - 0.05 * i});
            }
            const double r = std::pow(2.0, semis / 12.0);
            std::vector<double> targets;
            for (auto& n : notes) for (int h = 1; h <= 18; ++h) targets.push_back(n.f * h * std::sqrt(1 + 0.00012 * h * h) * r);
            DTXProcessor* p = makeProc(engine, semis);
            auto out = render(p, strum(notes, 2.2, 1.0));
            dtx_processor_destroy(p);
            sum += artifactDb(out, targets);
        }
        const double mean = sum / shapes.size();
        total += mean; ++count;
        if (mean > worst) { worst = mean; worstShift = semis; }
    }
    char msg[200];
    std::snprintf(msg, sizeof msg, "%-8s strummed chords: artifacts %6.1f dB mean (worst %6.1f dB at %+g st), limit %.0f dB",
                  engineName(engine), total / count, worst, worstShift, maxMeanDb);
    check(total / count <= maxMeanDb, msg);
}

int main() {
    std::puts("Pitch accuracy (single notes)");
    const float shifts[] = {-36, -24, -12, -7, -5, -1, -0.2f, 0.2f, 1, 2, 5, 7, 12, 24};
    for (int e = 0; e < 4; ++e)
        for (float s : shifts) {
            double f0 = s <= -24 ? 330.0 : (s >= 24 ? 110.0 : 196.0);
            double tol = std::fabs(s) < 1 ? 3.0 : (e == 0 && std::fabs(s) >= 24 ? 25.0 : 10.0);
            testPitch(e, f0, s, tol);
        }
    std::puts("Chord (polyphony)");
    const std::vector<double> eMaj = {164.81, 246.94, 329.63, 415.30};
    const std::vector<double> lowE = {82.41, 123.47, 164.81};          // E5 power chord, low strings
    for (int e = 0; e < 4; ++e) {
        for (float s : {-12.f, -5.f, -2.f, 0.15f, 4.f, 7.f, 12.f}) testChord(e, "E major", eMaj, s, 0.75);
        // Tight (short grains) is meant for single notes and detune, not for chords on the
        // lowest strings. Reported, not asserted, for Tight.
        for (float s : {-12.f, -2.f, 7.f}) testChord(e, "low E5", lowE, s, 0.9, e == 0);
    }
    std::puts("Strummed chords (realistic guitar model)");
    testStrummedChords(0, -16); testStrummedChords(1, -24); testStrummedChords(2, -28); testStrummedChords(3, -32);
    std::puts("Level stability");
    for (int e = 0; e < 4; ++e) for (float s : {-24.f, -12.f, -5.f, 0.2f, 7.f, 12.f}) testLevel(e, s);
    std::puts("Latency");
    for (int e = 0; e < 4; ++e) { testLatency(e, -12); testLatency(e, 0.2f); testLatency(e, 12); }
    std::puts("Control behaviour");
    testGlide();
    testBypass();
    std::puts("CPU");
    testCpu();
    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
