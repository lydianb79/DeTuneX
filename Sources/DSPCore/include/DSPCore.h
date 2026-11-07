// DSPCore.h — C interface to the De-TuneX realtime pitch-shifting processor.
//
// Threading model
//   * dtx_processor_prepare / _reset must only be called while audio is stopped.
//   * dtx_processor_process is called from the realtime audio thread only.
//   * dtx_set_param / dtx_get_* / dtx_read_scope are lock-free and may be called
//     from any thread (UI, MIDI) at any time.
#ifndef DTX_DSPCORE_H
#define DTX_DSPCORE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DTXProcessor DTXProcessor;

typedef enum {
    DTX_PARAM_ENGAGED = 0,          // 0/1   effect on (pedal state, decided by the UI/controller)
    DTX_PARAM_BALANCE,              // 0..1  0 = direct only, 1 = effect only
    DTX_PARAM_SEMITONES,            // -36..+36 target shift (detune is expressed as fractional semitones)
    DTX_PARAM_RISE_MS,              // 0..2000 glide time from 0 to target shift when engaging
    DTX_PARAM_FALL_MS,              // 0..2000 glide time back to 0 when disengaging
    DTX_PARAM_EXPRESSION,           // 0..1  expression pedal position (heel..toe)
    DTX_PARAM_EXPRESSION_ACTIVE,    // 0/1   when 1 the shift is scaled linearly by EXPRESSION
    DTX_PARAM_ENGINE,               // 0 Tight, 1 Balanced, 2 Wide/Bass, 3 Studio (spectral)
    DTX_PARAM_INPUT_GAIN_DB,        // -24..+24
    DTX_PARAM_OUTPUT_GAIN_DB,       // -24..+24
    DTX_PARAM_COUNT
} DTXParam;

typedef enum {
    DTX_METER_INPUT = 0,
    DTX_METER_OUTPUT = 1
} DTXMeter;

DTXProcessor* dtx_processor_create(void);
void  dtx_processor_destroy(DTXProcessor* p);
void  dtx_processor_prepare(DTXProcessor* p, double sampleRate, int maxFrames);
void  dtx_processor_reset(DTXProcessor* p);

// Mono in → mono out. `in` and `out` may alias.
void  dtx_processor_process(DTXProcessor* p, const float* in, float* out, int frames);

void  dtx_set_param(DTXProcessor* p, DTXParam param, float value);
float dtx_get_param(const DTXProcessor* p, DTXParam param);

// Live state for the UI.
float dtx_get_current_semitones(const DTXProcessor* p);   // actual (glided) shift
float dtx_get_wet_amount(const DTXProcessor* p);          // 0..1 effect engagement (for LED)
float dtx_get_latency_samples(const DTXProcessor* p);     // algorithmic latency of active engine
float dtx_take_meter_peak(DTXProcessor* p, DTXMeter which); // peak since last call (linear)

// Copies the most recent `count` samples (count <= 4096) of the dry input and wet output.
void  dtx_read_scope(const DTXProcessor* p, float* dry, float* wet, int count);

#ifdef __cplusplus
}
#endif
#endif
