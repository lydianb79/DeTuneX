// AudioHost.h — CoreAudio HAL (AUHAL) duplex host for the De-TuneX processor.
//
// Low-latency design:
//   * One AUHAL unit runs input and output in the *same* device IO cycle: the
//     render callback pulls the input buffer and writes the processed output
//     in one pass, so the round trip is in-latency + 2 x buffer + out-latency.
//   * If input and output are different devices, a private (hidden) aggregate
//     device is created on the fly, clocked by the output device with drift
//     compensation on the input, so the duplex single-callback path still applies.
//   * The device's IO buffer size and nominal sample rate are set directly on
//     the HAL device (not just the unit), which is what actually sets latency.
//   * No locks, allocation, Objective-C or Swift in the callback.
#ifndef DTX_AUDIOHOST_H
#define DTX_AUDIOHOST_H

#include <stdint.h>
#include "DSPCore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DTXHost DTXHost;

typedef struct {
    uint32_t inputDevice;      // AudioObjectID
    uint32_t outputDevice;     // AudioObjectID
    int32_t  inputChannel;     // 0-based channel on the input device
    int32_t  outputChannelL;   // 0-based channel on the output device
    int32_t  outputChannelR;   // second output channel, or -1 for mono
    double   sampleRate;       // requested nominal rate (0 = keep device rate)
    uint32_t bufferFrames;     // requested IO buffer size in frames
    float    ioCycleUsage;     // 0.1…1: fraction of the IO cycle reserved for processing
                               // (kAudioDevicePropertyIOCycleUsage). Lower = the HAL calls us
                               // later in the cycle = less output latency. 0 = leave as is.
} DTXHostConfig;

typedef struct {
    double   sampleRate;       // actual
    uint32_t bufferFrames;     // actual
    uint32_t inputLatencyFrames;   // device + stream latency + safety offset
    uint32_t outputLatencyFrames;  // device + stream latency + safety offset
    int32_t  usingAggregate;
    float    ioCycleUsage;     // actual value in effect
} DTXHostStatus;

DTXHost* dtx_host_create(DTXProcessor* processor);
void     dtx_host_destroy(DTXHost* host);

// Returns 0 on success; otherwise writes a message to errBuf.
int      dtx_host_start(DTXHost* host, const DTXHostConfig* cfg, DTXHostStatus* status,
                        char* errBuf, int errBufLen);
void     dtx_host_stop(DTXHost* host);
int      dtx_host_is_running(const DTXHost* host);

// Fraction of the IO period spent in the callback (smoothed peak, 0..1+).
float    dtx_host_dsp_load(DTXHost* host);
// Number of HAL processor overloads (dropouts) since start.
uint32_t dtx_host_overload_count(const DTXHost* host);
// Non-zero if the device died / was unplugged / reconfigured while running.
int      dtx_host_device_lost(const DTXHost* host);

#ifdef __cplusplus
}
#endif
#endif
