// AudioHost.cpp — AUHAL duplex host (macOS only).
#include "AudioHost.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr UInt32 kMaxFrames = 4096;

// ---------------------------------------------------------------------------
// HAL property helpers
// ---------------------------------------------------------------------------
AudioObjectPropertyAddress addr(AudioObjectPropertySelector sel,
                                AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal,
                                AudioObjectPropertyElement el = kAudioObjectPropertyElementMain) {
    return AudioObjectPropertyAddress{sel, scope, el};
}

template <typename T>
bool getProp(AudioObjectID obj, AudioObjectPropertyAddress a, T& out) {
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, &out) == noErr;
}

template <typename T>
OSStatus setProp(AudioObjectID obj, AudioObjectPropertyAddress a, const T& v) {
    return AudioObjectSetPropertyData(obj, &a, 0, nullptr, sizeof(T), &v);
}

UInt32 channelCount(AudioObjectID dev, AudioObjectPropertyScope scope) {
    auto a = addr(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr || size == 0) return 0;
    std::vector<uint8_t> mem(size);
    auto* abl = reinterpret_cast<AudioBufferList*>(mem.data());
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, abl) != noErr) return 0;
    UInt32 n = 0;
    for (UInt32 i = 0; i < abl->mNumberBuffers; ++i) n += abl->mBuffers[i].mNumberChannels;
    return n;
}

std::string deviceUID(AudioObjectID dev) {
    CFStringRef uid = nullptr;
    auto a = addr(kAudioDevicePropertyDeviceUID);
    UInt32 size = sizeof(uid);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &uid) != noErr || !uid) return {};
    char buf[512] = {0};
    CFStringGetCString(uid, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(uid);
    return buf;
}

UInt32 scopeLatency(AudioObjectID dev, AudioObjectPropertyScope scope) {
    UInt32 lat = 0, safety = 0, streamLat = 0;
    getProp(dev, addr(kAudioDevicePropertyLatency, scope), lat);
    getProp(dev, addr(kAudioDevicePropertySafetyOffset, scope), safety);
    // First stream's latency in this direction.
    auto a = addr(kAudioDevicePropertyStreams, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) == noErr && size >= sizeof(AudioStreamID)) {
        std::vector<AudioStreamID> streams(size / sizeof(AudioStreamID));
        if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, streams.data()) == noErr)
            getProp(streams[0], addr(kAudioStreamPropertyLatency), streamLat);
    }
    return lat + safety + streamLat;
}

bool waitFor(const std::function<bool()>& cond, int timeoutMs) {
    for (int t = 0; t < timeoutMs; t += 10) {
        if (cond()) return true;
        // The HAL updates properties on its own threads; just poll (no nested run loop).
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

bool setSampleRate(AudioObjectID dev, double rate) {
    Float64 cur = 0;
    getProp(dev, addr(kAudioDevicePropertyNominalSampleRate), cur);
    if (std::fabs(cur - rate) < 0.5) return true;
    Float64 r = rate;
    if (setProp(dev, addr(kAudioDevicePropertyNominalSampleRate), r) != noErr) return false;
    return waitFor([&] {
        Float64 now = 0;
        getProp(dev, addr(kAudioDevicePropertyNominalSampleRate), now);
        return std::fabs(now - rate) < 0.5;
    }, 1500);
}

UInt32 setBufferSize(AudioObjectID dev, UInt32 frames) {
    AudioValueRange range{0, 0};
    if (getProp(dev, addr(kAudioDevicePropertyBufferFrameSizeRange), range) && range.mMaximum > 0)
        frames = (UInt32)std::clamp<double>(frames, range.mMinimum, range.mMaximum);
    UInt32 f = frames;
    setProp(dev, addr(kAudioDevicePropertyBufferFrameSize), f);
    UInt32 actual = frames;
    getProp(dev, addr(kAudioDevicePropertyBufferFrameSize), actual);
    return actual;
}

// ---------------------------------------------------------------------------
// Private aggregate device (input device first, output device second)
// ---------------------------------------------------------------------------
AudioObjectID createAggregate(AudioObjectID inDev, AudioObjectID outDev, std::string& err) {
    const std::string inUID = deviceUID(inDev), outUID = deviceUID(outDev);
    if (inUID.empty() || outUID.empty()) { err = "Could not read device UIDs."; return kAudioObjectUnknown; }

    auto cfstr = [](const std::string& s) {
        return CFStringCreateWithCString(kCFAllocatorDefault, s.c_str(), kCFStringEncodingUTF8);
    };
    CFStringRef cfIn = cfstr(inUID), cfOut = cfstr(outUID);
    char aggUID[128];
    std::snprintf(aggUID, sizeof aggUID, "com.detunex.aggregate.%llu", (unsigned long long)mach_absolute_time());
    CFStringRef cfAggUID = cfstr(aggUID);
    CFStringRef cfName = CFSTR("De-TuneX Duplex");

    int one = 1, zero = 0;
    CFNumberRef cfOne = CFNumberCreate(nullptr, kCFNumberIntType, &one);
    CFNumberRef cfZero = CFNumberCreate(nullptr, kCFNumberIntType, &zero);

    // Sub-device descriptions. Drift compensation on the input; output is the clock.
    CFMutableDictionaryRef subIn = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(subIn, CFSTR(kAudioSubDeviceUIDKey), cfIn);
    CFDictionarySetValue(subIn, CFSTR(kAudioSubDeviceDriftCompensationKey), cfOne);
    CFMutableDictionaryRef subOut = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(subOut, CFSTR(kAudioSubDeviceUIDKey), cfOut);
    CFDictionarySetValue(subOut, CFSTR(kAudioSubDeviceDriftCompensationKey), cfZero);

    const void* subs[2] = {subIn, subOut};
    CFArrayRef subList = CFArrayCreate(nullptr, subs, 2, &kCFTypeArrayCallBacks);

    CFMutableDictionaryRef desc = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceUIDKey), cfAggUID);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceNameKey), cfName);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsPrivateKey), cfOne);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsStackedKey), cfZero);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceSubDeviceListKey), subList);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceMainSubDeviceKey), cfOut);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceClockDeviceKey), cfOut);

    AudioObjectID agg = kAudioObjectUnknown;
    OSStatus st = AudioHardwareCreateAggregateDevice(desc, &agg);

    CFRelease(desc); CFRelease(subList); CFRelease(subIn); CFRelease(subOut);
    CFRelease(cfOne); CFRelease(cfZero); CFRelease(cfAggUID); CFRelease(cfIn); CFRelease(cfOut);

    if (st != noErr || agg == kAudioObjectUnknown) {
        err = "Could not create aggregate device (OSStatus " + std::to_string((int)st) + ").";
        return kAudioObjectUnknown;
    }
    // The aggregate's streams appear asynchronously.
    const UInt32 wantOut = channelCount(inDev, kAudioObjectPropertyScopeOutput) + channelCount(outDev, kAudioObjectPropertyScopeOutput);
    waitFor([&] {
        return channelCount(agg, kAudioObjectPropertyScopeInput) >= channelCount(inDev, kAudioObjectPropertyScopeInput) &&
               channelCount(agg, kAudioObjectPropertyScopeOutput) >= wantOut;
    }, 2000);
    return agg;
}

} // namespace

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------
struct DTXHost {
    DTXProcessor* proc = nullptr;
    AudioUnit au = nullptr;
    AudioObjectID device = kAudioObjectUnknown;     // device the AUHAL runs on
    AudioObjectID aggregate = kAudioObjectUnknown;  // non-zero if we created one
    std::vector<AudioObjectID> listenedDevices;

    // Pre-allocated input buffers (non-interleaved, one per device input channel).
    AudioBufferList* inABL = nullptr;
    std::vector<float> inMem;
    UInt32 inChannels = 0, outChannels = 0;
    int inCh = 0, outL = 0, outR = -1;

    std::atomic<bool> running{false};
    std::atomic<bool> deviceLost{false};
    std::atomic<uint32_t> overloads{0};
    std::atomic<float> load{0.f};
    std::atomic<uint64_t> armTime{0};   // ignore config notifications fired by our own setup
    double ticksToSec = 1.0;
    double periodSec = 0.001;

    static OSStatus render(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts,
                           UInt32 /*bus*/, UInt32 frames, AudioBufferList* io) {
        auto* h = static_cast<DTXHost*>(ref);
        const uint64_t t0 = mach_absolute_time();

        // Zero every output channel first; we only write the selected ones.
        for (UInt32 b = 0; b < io->mNumberBuffers; ++b)
            std::memset(io->mBuffers[b].mData, 0, io->mBuffers[b].mDataByteSize);
        if (frames > kMaxFrames) return noErr;

        for (UInt32 c = 0; c < h->inABL->mNumberBuffers; ++c)
            h->inABL->mBuffers[c].mDataByteSize = frames * sizeof(float);
        if (AudioUnitRender(h->au, flags, ts, 1, frames, h->inABL) != noErr) return noErr;

        const float* in = static_cast<const float*>(h->inABL->mBuffers[h->inCh].mData);
        if ((UInt32)h->outL < io->mNumberBuffers) {
            float* l = static_cast<float*>(io->mBuffers[h->outL].mData);
            dtx_processor_process(h->proc, in, l, (int)frames);
            if (h->outR >= 0 && (UInt32)h->outR < io->mNumberBuffers)
                std::memcpy(io->mBuffers[h->outR].mData, l, frames * sizeof(float));
        }

        const double used = (double)(mach_absolute_time() - t0) * h->ticksToSec;
        const float frac = (float)(used / (h->periodSec > 0 ? h->periodSec * frames : 1e-3));
        float cur = h->load.load(std::memory_order_relaxed);
        h->load.store(frac > cur ? frac : cur * 0.995f + frac * 0.005f, std::memory_order_relaxed);
        return noErr;
    }

    static OSStatus deviceListener(AudioObjectID, UInt32 n, const AudioObjectPropertyAddress* addrs, void* ref) {
        auto* h = static_cast<DTXHost*>(ref);
        for (UInt32 i = 0; i < n; ++i) {
            switch (addrs[i].mSelector) {
                case kAudioDeviceProcessorOverload: h->overloads.fetch_add(1); break;
                case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyNominalSampleRate:
                case kAudioDevicePropertyStreamConfiguration:
                    if (h->running.load() && mach_absolute_time() > h->armTime.load()) h->deviceLost.store(true);
                    break;
                default: break;
            }
        }
        return noErr;
    }

    static const AudioObjectPropertySelector* listenedSelectors(int& n) {
        static const AudioObjectPropertySelector s[] = {
            kAudioDeviceProcessorOverload, kAudioDevicePropertyDeviceIsAlive,
            kAudioDevicePropertyNominalSampleRate, kAudioDevicePropertyStreamConfiguration};
        n = 4;
        return s;
    }

    void addListeners(AudioObjectID dev) {
        int n; auto* sel = listenedSelectors(n);
        for (int i = 0; i < n; ++i) {
            auto a = addr(sel[i], kAudioObjectPropertyScopeWildcard, kAudioObjectPropertyElementWildcard);
            AudioObjectAddPropertyListener(dev, &a, deviceListener, this);
        }
        listenedDevices.push_back(dev);
    }

    void removeListeners() {
        int n; auto* sel = listenedSelectors(n);
        for (auto dev : listenedDevices)
            for (int i = 0; i < n; ++i) {
                auto a = addr(sel[i], kAudioObjectPropertyScopeWildcard, kAudioObjectPropertyElementWildcard);
                AudioObjectRemovePropertyListener(dev, &a, deviceListener, this);
            }
        listenedDevices.clear();
    }

    void freeBuffers() {
        std::free(inABL);
        inABL = nullptr;
        inMem.clear();
        inMem.shrink_to_fit();
    }

    void stop() {
        running.store(false);
        if (au) {
            AudioOutputUnitStop(au);
            AudioUnitUninitialize(au);
            AudioComponentInstanceDispose(au);
            au = nullptr;
        }
        removeListeners();
        if (aggregate != kAudioObjectUnknown) {
            AudioHardwareDestroyAggregateDevice(aggregate);
            aggregate = kAudioObjectUnknown;
        }
        freeBuffers();
        device = kAudioObjectUnknown;
    }

    int start(const DTXHostConfig& cfg, DTXHostStatus* status, std::string& err) {
        stop();
        deviceLost.store(false);
        overloads.store(0);
        load.store(0.f);

        if (cfg.inputDevice == kAudioObjectUnknown || cfg.outputDevice == kAudioObjectUnknown) {
            err = "Select an input and an output device.";
            return -1;
        }
        const UInt32 devIn = channelCount(cfg.inputDevice, kAudioObjectPropertyScopeInput);
        const UInt32 devOut = channelCount(cfg.outputDevice, kAudioObjectPropertyScopeOutput);
        if (cfg.inputChannel < 0 || (UInt32)cfg.inputChannel >= devIn) { err = "Input channel not available on this device."; return -1; }
        if (cfg.outputChannelL < 0 || (UInt32)cfg.outputChannelL >= devOut) { err = "Output channel not available on this device."; return -1; }

        // Bring both devices to the requested rate before aggregating (avoids
        // resampling inside the aggregate and keeps latency deterministic).
        if (cfg.sampleRate > 0) {
            if (!setSampleRate(cfg.outputDevice, cfg.sampleRate)) { err = "Output device does not support this sample rate."; return -1; }
            if (cfg.inputDevice != cfg.outputDevice && !setSampleRate(cfg.inputDevice, cfg.sampleRate)) {
                err = "Input device does not support this sample rate."; return -1;
            }
        }

        int inOffset = 0, outOffset = 0;
        if (cfg.inputDevice == cfg.outputDevice) {
            device = cfg.inputDevice;
        } else {
            aggregate = createAggregate(cfg.inputDevice, cfg.outputDevice, err);
            if (aggregate == kAudioObjectUnknown) return -1;
            device = aggregate;
            outOffset = (int)channelCount(cfg.inputDevice, kAudioObjectPropertyScopeOutput);
            // Sub-devices share the IO cycle: set their buffer size too.
            setBufferSize(cfg.inputDevice, cfg.bufferFrames);
            setBufferSize(cfg.outputDevice, cfg.bufferFrames);
        }
        inCh = cfg.inputChannel + inOffset;
        outL = cfg.outputChannelL + outOffset;
        outR = cfg.outputChannelR >= 0 ? cfg.outputChannelR + outOffset : -1;

        const UInt32 frames = setBufferSize(device, std::max<UInt32>(cfg.bufferFrames, 8));

        // IO cycle usage: by default the HAL wakes our IO proc at the start of the
        // cycle and reserves the whole cycle for it. Our DSP needs only a few % of
        // it, so telling the HAL we use less makes it call us later, which takes
        // up to (1 - usage) x buffer off the output latency at no CPU cost.
        if (cfg.ioCycleUsage > 0.f) {
            Float32 usage = std::clamp(cfg.ioCycleUsage, 0.1f, 1.f);
            setProp(device, addr(kAudioDevicePropertyIOCycleUsage), usage);
        }
        Float64 rate = 48000;
        getProp(device, addr(kAudioDevicePropertyNominalSampleRate), rate);

        // --- AUHAL ---
        AudioComponentDescription d{kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
        AudioComponent comp = AudioComponentFindNext(nullptr, &d);
        if (!comp || AudioComponentInstanceNew(comp, &au) != noErr) { err = "Could not create AUHAL."; au = nullptr; stop(); return -1; }

        auto fail = [&](const char* what, OSStatus s) {
            err = std::string(what) + " (OSStatus " + std::to_string((int)s) + ").";
            stop();
            return -1;
        };

        UInt32 one = 1;
        OSStatus s;
        if ((s = AudioUnitSetProperty(au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one, sizeof one))) return fail("Enable input", s);
        if ((s = AudioUnitSetProperty(au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &one, sizeof one))) return fail("Enable output", s);
        if ((s = AudioUnitSetProperty(au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, sizeof device))) return fail("Set device", s);

        AudioStreamBasicDescription hwIn{}, hwOut{};
        UInt32 sz = sizeof hwIn;
        AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &hwIn, &sz);
        sz = sizeof hwOut;
        AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &hwOut, &sz);
        inChannels = hwIn.mChannelsPerFrame;
        outChannels = hwOut.mChannelsPerFrame;
        if (inChannels == 0 || (UInt32)inCh >= inChannels) return fail("Input channel out of range on the running device", -1);
        if (outChannels == 0 || (UInt32)outL >= outChannels) return fail("Output channel out of range on the running device", -1);
        if (outR >= (int)outChannels) outR = -1;

        auto clientFormat = [&](UInt32 ch) {
            AudioStreamBasicDescription f{};
            f.mSampleRate = rate;
            f.mFormatID = kAudioFormatLinearPCM;
            f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
            f.mBytesPerPacket = f.mBytesPerFrame = sizeof(float);
            f.mFramesPerPacket = 1;
            f.mChannelsPerFrame = ch;
            f.mBitsPerChannel = 32;
            return f;
        };
        AudioStreamBasicDescription cin = clientFormat(inChannels), cout = clientFormat(outChannels);
        if ((s = AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &cin, sizeof cin))) return fail("Input format", s);
        if ((s = AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &cout, sizeof cout))) return fail("Output format", s);

        UInt32 maxSlice = kMaxFrames;
        AudioUnitSetProperty(au, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxSlice, sizeof maxSlice);

        // Input buffers.
        const size_t ablSize = offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer) * inChannels;
        inABL = static_cast<AudioBufferList*>(std::calloc(1, ablSize));
        inMem.assign((size_t)inChannels * kMaxFrames, 0.f);
        inABL->mNumberBuffers = inChannels;
        for (UInt32 c = 0; c < inChannels; ++c) {
            inABL->mBuffers[c].mNumberChannels = 1;
            inABL->mBuffers[c].mDataByteSize = kMaxFrames * sizeof(float);
            inABL->mBuffers[c].mData = inMem.data() + (size_t)c * kMaxFrames;
        }

        AURenderCallbackStruct cb{&DTXHost::render, this};
        if ((s = AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb))) return fail("Render callback", s);

        dtx_processor_prepare(proc, rate, (int)kMaxFrames);

        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        ticksToSec = (double)tb.numer / (double)tb.denom * 1e-9;
        periodSec = 1.0 / rate;

        if ((s = AudioUnitInitialize(au))) return fail("Initialize AUHAL", s);

        addListeners(device);
        if (aggregate != kAudioObjectUnknown) { addListeners(cfg.inputDevice); addListeners(cfg.outputDevice); }

        armTime.store(mach_absolute_time() + (uint64_t)(1.5 / ticksToSec));
        running.store(true);
        if ((s = AudioOutputUnitStart(au))) { running.store(false); return fail("Start audio", s); }

        if (status) {
            status->sampleRate = rate;
            UInt32 actual = frames;
            getProp(device, addr(kAudioDevicePropertyBufferFrameSize), actual);
            status->bufferFrames = actual;
            status->inputLatencyFrames = scopeLatency(device, kAudioObjectPropertyScopeInput);
            status->outputLatencyFrames = scopeLatency(device, kAudioObjectPropertyScopeOutput);
            status->usingAggregate = aggregate != kAudioObjectUnknown;
            Float32 usage = 1.f;
            getProp(device, addr(kAudioDevicePropertyIOCycleUsage), usage);
            status->ioCycleUsage = usage;
        }
        return 0;
    }
};

extern "C" {

DTXHost* dtx_host_create(DTXProcessor* processor) {
    auto* h = new DTXHost();
    h->proc = processor;
    return h;
}

void dtx_host_destroy(DTXHost* h) {
    if (!h) return;
    h->stop();
    delete h;
}

int dtx_host_start(DTXHost* h, const DTXHostConfig* cfg, DTXHostStatus* status, char* errBuf, int errBufLen) {
    std::string err;
    const int r = h->start(*cfg, status, err);
    if (r != 0 && errBuf && errBufLen > 0) {
        std::snprintf(errBuf, (size_t)errBufLen, "%s", err.c_str());
    }
    return r;
}

void dtx_host_stop(DTXHost* h) { h->stop(); }
int dtx_host_is_running(const DTXHost* h) { return h->running.load() ? 1 : 0; }

float dtx_host_dsp_load(DTXHost* h) { return h->load.load(std::memory_order_relaxed); }
uint32_t dtx_host_overload_count(const DTXHost* h) { return h->overloads.load(); }
int dtx_host_device_lost(const DTXHost* h) { return h->deviceLost.load() ? 1 : 0; }

} // extern "C"
