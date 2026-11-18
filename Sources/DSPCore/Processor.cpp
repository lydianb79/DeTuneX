// Processor.cpp — De-TuneX signal flow, modelled on the XS-1 Poly Shifter:
//
//   in ─ gain ─┬──────────────────────────── direct ──┐
//              └─ shifter (engine) ─ effect ─ BALANCE ┴─ bypass xfade ─ out gain ─ soft limit ─ out
//
//   shift (semitones) = SHIFT * glide(rise/fall) * (expression ? EXP : 1)
//
// Everything here is allocation-free and lock-free on the audio thread.
#include "DSPCore.h"
#include "GrainShifter.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

namespace {

// Flush denormals to zero for the duration of a callback.
struct ScopedFlushDenormals {
#if defined(__aarch64__)
    uint64_t saved;
    ScopedFlushDenormals() {
        asm volatile("mrs %0, fpcr" : "=r"(saved));
        asm volatile("msr fpcr, %0" ::"r"(saved | (1ULL << 24)));
    }
    ~ScopedFlushDenormals() { asm volatile("msr fpcr, %0" ::"r"(saved)); }
#elif defined(__x86_64__) || defined(_M_X64)
    unsigned int saved;
    ScopedFlushDenormals() {
        saved = _mm_getcsr();
        _mm_setcsr(saved | 0x8040); // FTZ | DAZ
    }
    ~ScopedFlushDenormals() { _mm_setcsr(saved); }
#endif
};

inline float dbToGain(float db) { return std::pow(10.f, db / 20.f); }

inline float softLimit(float x) {
    const float a = std::fabs(x);
    if (a <= 0.9f) return x;
    const float y = 0.9f + 0.1f * std::tanh((a - 0.9f) * 10.f);
    return x < 0 ? -y : y;
}

constexpr int kScopeSize = 4096;

} // namespace

struct DTXProcessor {
    // ---- shared with other threads -------------------------------------
    std::atomic<float> params[DTX_PARAM_COUNT];
    std::atomic<float> curSemis{0.f}, wetAmount{0.f}, latency{0.f};
    std::atomic<float> peakIn{0.f}, peakOut{0.f};
    std::atomic<float> scopeDry[kScopeSize], scopeWet[kScopeSize];
    std::atomic<int> scopeWrite{0};

    // Diagnostic recorder: raw input and final output, sample-exact.
    // Buffers are (re)allocated only while diagState == 0 (idle) or 2 (done),
    // i.e. never while the audio thread writes them.
    std::atomic<int> diagState{0};   // 0 idle, 1 recording, 2 done
    std::atomic<int> diagPos{0};
    int diagLen = 0;
    std::vector<float> diagIn, diagOut;

    // ---- audio thread state ---------------------------------------------
    double sr = 48000.0;
    dtx::GrainShifter grain;
    int currentGrainMode = -1;
    int currentFastAttack = -1;

    float glide = 0.f;          // 0..1 glide position
    float semisSmooth = 0.f;    // smoothed effective shift
    float exprSmooth = 1.f;
    float bypassGain = 0.f;     // 0 = direct/bypass, 1 = effect path
    float inGain = 1.f, outGain = 1.f, balDry = 0.f, balWet = 1.f;

    DTXProcessor() {
        const float defaults[DTX_PARAM_COUNT] = {
            0.f,    // ENGAGED
            1.f,    // BALANCE (effect only, as the XS-1 suggests for semitone shifts)
            -1.f,   // SEMITONES
            0.f,    // RISE
            0.f,    // FALL
            1.f,    // EXPRESSION
            0.f,    // EXPRESSION_ACTIVE
            1.f,    // ENGINE (Balanced)
            0.f,    // INPUT GAIN
            0.f,    // OUTPUT GAIN
            1.f     // FAST ATTACK
        };
        for (int i = 0; i < DTX_PARAM_COUNT; ++i) params[i].store(defaults[i]);
        for (int i = 0; i < kScopeSize; ++i) { scopeDry[i].store(0.f); scopeWet[i].store(0.f); }
        grain.prepare();
    }

    float p(DTXParam id) const { return params[id].load(std::memory_order_relaxed); }

    dtx::GrainConfig grainConfigFor(int mode, bool fastAttack) const {
        const double s = sr / 48000.0;
        auto n = [s](double v) { return std::max(1, (int)std::lround(v * s)); };
        dtx::GrainConfig c = baseConfig(mode, n);
        c.snap = fastAttack;
        c.snapFade = n(96);
        c.rateScale = (float)s;
        return c;
    }

    template <typename N>
    static dtx::GrainConfig baseConfig(int mode, N n) {
        switch (mode) {
            // grain, search radius, correlation length, tolerance.
            // Chord artifact level / shifter delay on strummed chords (48 kHz, see Tests):
            case 0:  return { n(512),  n(500), n(512),  0.02f };  // Tight:    single notes, detune  (-18 dB, ~10 ms)
            case 2:  return { n(2048), n(900), n(1024), 0.02f };  // Wide:     chords, bass, drop    (-30 dB, ~19 ms)
            case 3:  return { n(8192), n(900), n(1024), 0.02f };  // Studio:   best chords           (-34 dB, ~36 ms)
            default: return { n(1024), n(600), n(1024), 0.02f };  // Balanced: default               (-26 dB, ~13 ms)
        }
    }

    void prepare(double sampleRate) {
        sr = sampleRate > 0 ? sampleRate : 48000.0;
        grain.prepare();
        currentGrainMode = -1;
        currentFastAttack = -1;
        reset();
    }

    void reset() {
        grain.reset();
        glide = 0.f; semisSmooth = 0.f; exprSmooth = 1.f;
        bypassGain = p(DTX_PARAM_ENGAGED) > 0.5f ? 1.f : 0.f;
    }

    void process(const float* in, float* out, int frames) {
        ScopedFlushDenormals ftz;

        // --- block-rate parameter reads ---
        const bool engaged = p(DTX_PARAM_ENGAGED) > 0.5f;
        const float targetSemis = std::clamp(p(DTX_PARAM_SEMITONES), -36.f, 36.f);
        const float riseMs = std::clamp(p(DTX_PARAM_RISE_MS), 0.f, 5000.f);
        const float fallMs = std::clamp(p(DTX_PARAM_FALL_MS), 0.f, 5000.f);
        const bool exprActive = p(DTX_PARAM_EXPRESSION_ACTIVE) > 0.5f;
        const float exprTarget = exprActive ? std::clamp(p(DTX_PARAM_EXPRESSION), 0.f, 1.f) : 1.f;
        const int engine = std::clamp((int)std::lround(p(DTX_PARAM_ENGINE)), 0, 3);
        const float bal = std::clamp(p(DTX_PARAM_BALANCE), 0.f, 1.f);

        const float inGainT = dbToGain(std::clamp(p(DTX_PARAM_INPUT_GAIN_DB), -24.f, 24.f));
        const float outGainT = dbToGain(std::clamp(p(DTX_PARAM_OUTPUT_GAIN_DB), -24.f, 24.f));
        // Constant-power balance law.
        const float balDryT = std::cos(bal * (float)M_PI * 0.5f);
        const float balWetT = std::sin(bal * (float)M_PI * 0.5f);

        const int grainMode = engine;   // all engines are configurations of the grain shifter
        const int fastAttack = p(DTX_PARAM_FAST_ATTACK) > 0.5f ? 1 : 0;
        if (grainMode != currentGrainMode || fastAttack != currentFastAttack) {
            grain.setConfig(grainConfigFor(grainMode, fastAttack != 0));
            currentGrainMode = grainMode;
            currentFastAttack = fastAttack;
        }

        // --- per-sample coefficients ---
        const float fs = (float)sr;
        const float riseStep = riseMs > 0.5f ? 1.f / (riseMs * 0.001f * fs) : 1.f;
        const float fallStep = fallMs > 0.5f ? 1.f / (fallMs * 0.001f * fs) : 1.f;
        const float bypassStep = 1.f / (0.006f * fs);           // 6 ms click-free switching
        const float semisCoef = 1.f - std::exp(-1.f / (0.012f * fs)); // knob/MIDI smoothing
        const float exprCoef = 1.f - std::exp(-1.f / (0.008f * fs));
        const float paramCoef = 1.f - std::exp(-1.f / (0.010f * fs));

        float pkIn = 0.f, pkOut = 0.f;
        int sw = scopeWrite.load(std::memory_order_relaxed);
        const bool diag = diagState.load(std::memory_order_acquire) == 1;
        int dpos = diag ? diagPos.load(std::memory_order_relaxed) : 0;

        for (int i = 0; i < frames; ++i) {
            inGain += (inGainT - inGain) * paramCoef;
            outGain += (outGainT - outGain) * paramCoef;
            balDry += (balDryT - balDry) * paramCoef;
            balWet += (balWetT - balWet) * paramCoef;

            const float x = in[i] * inGain;

            // Glide (rise when engaged, fall when released).
            if (engaged) glide = std::min(1.f, glide + riseStep);
            else         glide = std::max(0.f, glide - fallStep);

            exprSmooth += (exprTarget - exprSmooth) * exprCoef;
            const float semisT = targetSemis * glide * exprSmooth;
            semisSmooth += (semisT - semisSmooth) * semisCoef;
            const float ratio = std::exp2(semisSmooth / 12.f);

            // Effect path stays engaged while the fall glide is still running.
            const bool wetActive = engaged || glide > 0.f;
            if (wetActive) bypassGain = std::min(1.f, bypassGain + bypassStep);
            else           bypassGain = std::max(0.f, bypassGain - bypassStep);

            // Engine changes take effect at the next grain boundary (click-free).
            const float shifted = grain.process(x, ratio);

            const float effect = balDry * x + balWet * shifted;
            float y = x + (effect - x) * bypassGain;
            y = softLimit(y * outGain);
            out[i] = y;
            if (diag && dpos < diagLen) { diagIn[dpos] = in[i]; diagOut[dpos] = y; ++dpos; }

            pkIn = std::max(pkIn, std::fabs(x));
            pkOut = std::max(pkOut, std::fabs(y));
            scopeDry[sw].store(x, std::memory_order_relaxed);
            scopeWet[sw].store(y, std::memory_order_relaxed);
            if (++sw == kScopeSize) sw = 0;
        }

        scopeWrite.store(sw, std::memory_order_release);
        if (diag) {
            diagPos.store(dpos, std::memory_order_relaxed);
            if (dpos >= diagLen) diagState.store(2, std::memory_order_release);
        }
        curSemis.store(semisSmooth, std::memory_order_relaxed);
        wetAmount.store(bypassGain, std::memory_order_relaxed);
        latency.store(grain.latency(), std::memory_order_relaxed);
        atomicMax(peakIn, pkIn);
        atomicMax(peakOut, pkOut);
    }

    static void atomicMax(std::atomic<float>& a, float v) {
        float cur = a.load(std::memory_order_relaxed);
        while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    }
};

// ---------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------
extern "C" {

DTXProcessor* dtx_processor_create(void) { return new DTXProcessor(); }
void dtx_processor_destroy(DTXProcessor* p) { delete p; }
void dtx_processor_prepare(DTXProcessor* p, double sampleRate, int /*maxFrames*/) { p->prepare(sampleRate); }
void dtx_processor_reset(DTXProcessor* p) { p->reset(); }

void dtx_processor_process(DTXProcessor* p, const float* in, float* out, int frames) {
    if (frames > 0) p->process(in, out, frames);
}

void dtx_set_param(DTXProcessor* p, DTXParam param, float value) {
    if (param >= 0 && param < DTX_PARAM_COUNT && std::isfinite(value))
        p->params[param].store(value, std::memory_order_relaxed);
}

float dtx_get_param(const DTXProcessor* p, DTXParam param) {
    if (param < 0 || param >= DTX_PARAM_COUNT) return 0.f;
    return p->params[param].load(std::memory_order_relaxed);
}

float dtx_get_current_semitones(const DTXProcessor* p) { return p->curSemis.load(std::memory_order_relaxed); }
float dtx_get_wet_amount(const DTXProcessor* p) { return p->wetAmount.load(std::memory_order_relaxed); }
float dtx_get_latency_samples(const DTXProcessor* p) { return p->latency.load(std::memory_order_relaxed); }

float dtx_take_meter_peak(DTXProcessor* p, DTXMeter which) {
    auto& a = which == DTX_METER_INPUT ? p->peakIn : p->peakOut;
    return a.exchange(0.f, std::memory_order_relaxed);
}

int dtx_diag_begin(DTXProcessor* p, int frames) {
    if (frames <= 0 || p->diagState.load(std::memory_order_acquire) == 1) return -1;
    p->diagIn.assign((size_t)frames, 0.f);
    p->diagOut.assign((size_t)frames, 0.f);
    p->diagLen = frames;
    p->diagPos.store(0, std::memory_order_relaxed);
    p->diagState.store(1, std::memory_order_release);
    return 0;
}

int dtx_diag_state(const DTXProcessor* p, int* framesDone) {
    if (framesDone) *framesDone = p->diagPos.load(std::memory_order_relaxed);
    return p->diagState.load(std::memory_order_acquire);
}

int dtx_diag_copy(DTXProcessor* p, float* in, float* out, int maxFrames) {
    if (p->diagState.load(std::memory_order_acquire) != 2) return 0;
    const int n = std::min(maxFrames, p->diagLen);
    if (in) std::copy(p->diagIn.begin(), p->diagIn.begin() + n, in);
    if (out) std::copy(p->diagOut.begin(), p->diagOut.begin() + n, out);
    return n;
}

void dtx_diag_reset(DTXProcessor* p) {
    if (p->diagState.load(std::memory_order_acquire) == 1) return;
    p->diagState.store(0, std::memory_order_release);
    p->diagLen = 0;
    std::vector<float>().swap(p->diagIn);
    std::vector<float>().swap(p->diagOut);
}

void dtx_read_scope(const DTXProcessor* p, float* dry, float* wet, int count) {
    count = std::clamp(count, 0, kScopeSize);
    int start = p->scopeWrite.load(std::memory_order_acquire) - count;
    if (start < 0) start += kScopeSize;
    for (int i = 0; i < count; ++i) {
        int idx = start + i;
        if (idx >= kScopeSize) idx -= kScopeSize;
        if (dry) dry[i] = p->scopeDry[idx].load(std::memory_order_relaxed);
        if (wet) wet[i] = p->scopeWet[idx].load(std::memory_order_relaxed);
    }
}

} // extern "C"
