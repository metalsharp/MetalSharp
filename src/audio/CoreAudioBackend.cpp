#include <metalsharp/CoreAudioBackend.h>
#include <metalsharp/Logger.h>

#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace metalsharp {

static constexpr uint32_t RING_BUFFER_FRAMES = 4096;
static constexpr uint32_t RING_BUFFER_CHANNELS = 2;
static constexpr uint32_t RING_BUFFER_BYTES = RING_BUFFER_FRAMES * RING_BUFFER_CHANNELS * sizeof(float);
static constexpr uint32_t PRE_BUFFER_FRAMES = 2048;
static constexpr uint32_t IO_BUFFER_FRAMES = 1024;
static constexpr uint32_t FADE_OUT_FRAMES = 64;

struct CoreAudioBackend::Impl {
    AudioUnit audioUnit = nullptr;
    AudioStreamBasicDescription asbd = {};

    std::atomic<bool> active{false};
    std::atomic<bool> playing{false};
    std::atomic<bool> preBuffering{false};
    std::atomic<float> volume{1.0f};
    std::atomic<float> frequencyRatio{1.0f};

    // Format state is initialized by the game thread and consumed by the
    // real-time render thread. Keep the individual values atomic because the
    // first buffer can be submitted while the callback is starting up.
    std::atomic<uint32_t> bytesPerSample{2};
    std::atomic<uint32_t> channels{2};
    std::atomic<uint32_t> sampleRate{44100};
    std::atomic<bool> formatFrozen{false};
    XAudio2WaveFormat format = {};

    alignas(64) uint8_t ringBuffer[RING_BUFFER_BYTES] = {};
    alignas(64) std::atomic<uint32_t> writePos{0};
    alignas(64) std::atomic<uint32_t> readPos{0};

    alignas(64) std::atomic<uint32_t> queuedCount{0};
    alignas(64) std::atomic<uint32_t> underrunCount{0};
    std::atomic<uint32_t> fadeOutRemaining{0};
    std::atomic<float> fadeOutGain{1.0f};

    uint32_t ringFramesAvailable() const {
        uint32_t w = writePos.load(std::memory_order_acquire);
        uint32_t r = readPos.load(std::memory_order_acquire);
        uint32_t total = static_cast<uint32_t>(RING_BUFFER_FRAMES * RING_BUFFER_CHANNELS * sizeof(float));
        uint32_t channelCount = channels.load(std::memory_order_acquire);
        uint32_t frameBytes = static_cast<uint32_t>(channelCount * sizeof(float));
        return frameBytes > 0 ? ((w + total - r) % total) / frameBytes : 0;
    }

    uint32_t ringFramesFree() const {
        uint32_t w = writePos.load(std::memory_order_acquire);
        uint32_t r = readPos.load(std::memory_order_acquire);
        uint32_t total = static_cast<uint32_t>(RING_BUFFER_FRAMES * RING_BUFFER_CHANNELS * sizeof(float));
        uint32_t channelCount = channels.load(std::memory_order_acquire);
        uint32_t frameBytes = static_cast<uint32_t>(channelCount * sizeof(float));
        return frameBytes > 0 ? ((r + total - w - 1) % total) / frameBytes : 0;
    }

    uint32_t ringRead(float* out, uint32_t frames) {
        uint32_t total = static_cast<uint32_t>(RING_BUFFER_FRAMES * RING_BUFFER_CHANNELS * sizeof(float));
        uint32_t channelCount = channels.load(std::memory_order_acquire);
        size_t frameBytes = static_cast<size_t>(frames) * channelCount * sizeof(float);
        uint32_t r = readPos.load(std::memory_order_acquire);
        size_t first = std::min(frameBytes, static_cast<size_t>(total - r));
        memcpy(out, ringBuffer + r, first);
        if (first < frameBytes) {
            memcpy(reinterpret_cast<uint8_t*>(out) + first, ringBuffer, frameBytes - first);
        }
        readPos.store(static_cast<uint32_t>((r + frameBytes) % total), std::memory_order_release);
        return frames;
    }

    uint32_t ringWrite(const float* in, uint32_t frames) {
        uint32_t total = static_cast<uint32_t>(RING_BUFFER_FRAMES * RING_BUFFER_CHANNELS * sizeof(float));
        uint32_t channelCount = channels.load(std::memory_order_acquire);
        size_t frameBytes = static_cast<size_t>(frames) * channelCount * sizeof(float);
        uint32_t w = writePos.load(std::memory_order_acquire);
        size_t first = std::min(frameBytes, static_cast<size_t>(total - w));
        memcpy(ringBuffer + w, reinterpret_cast<const uint8_t*>(in), first);
        if (first < frameBytes) {
            memcpy(ringBuffer, reinterpret_cast<const uint8_t*>(in) + first, frameBytes - first);
        }
        writePos.store(static_cast<uint32_t>((w + frameBytes) % total), std::memory_order_release);
        return frames;
    }
};

static void convertInt16ToFloat(const int16_t* src, float* dst, uint32_t sampleCount) {
    for (uint32_t i = 0; i < sampleCount; i++) {
        dst[i] = static_cast<float>(src[i]) / 32768.0f;
    }
}

static void convertInt8ToFloat(const int8_t* src, float* dst, uint32_t sampleCount) {
    for (uint32_t i = 0; i < sampleCount; i++) {
        dst[i] = static_cast<float>(src[i]) / 128.0f;
    }
}

static void convertUInt8ToFloat(const uint8_t* src, float* dst, uint32_t sampleCount) {
    for (uint32_t i = 0; i < sampleCount; i++) {
        dst[i] = (static_cast<float>(src[i]) - 128.0f) / 128.0f;
    }
}

static void convertInt24ToFloat(const uint8_t* src, float* dst, uint32_t sampleCount) {
    for (uint32_t i = 0; i < sampleCount; i++) {
        int32_t v = (static_cast<int32_t>(src[i * 3 + 2]) << 24) | (static_cast<int32_t>(src[i * 3 + 1]) << 16) |
                    (static_cast<int32_t>(src[i * 3]) << 8);
        dst[i] = static_cast<float>(v) / 2147483648.0f;
    }
}

static void convertInt32ToFloat(const int32_t* src, float* dst, uint32_t sampleCount) {
    for (uint32_t i = 0; i < sampleCount; i++) {
        dst[i] = static_cast<float>(src[i]) / 2147483648.0f;
    }
}

static void convertFloatToFloat(const float* src, float* dst, uint32_t sampleCount) {
    memcpy(dst, src, sampleCount * sizeof(float));
}

static OSStatus audioRenderCallback(void* inRefCon, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32,
                                    UInt32 inNumberFrames, AudioBufferList* ioData) {
    auto* impl = static_cast<CoreAudioBackend::Impl*>(inRefCon);

    if (!impl->playing.load(std::memory_order_acquire)) {
        for (UInt32 i = 0; i < ioData->mNumberBuffers; i++) {
            memset(ioData->mBuffers[i].mData, 0, ioData->mBuffers[i].mDataByteSize);
        }
        return noErr;
    }

    if (impl->preBuffering.load(std::memory_order_acquire)) {
        uint32_t avail = impl->ringFramesAvailable();
        if (avail < PRE_BUFFER_FRAMES) {
            for (UInt32 i = 0; i < ioData->mNumberBuffers; i++) {
                memset(ioData->mBuffers[i].mData, 0, ioData->mBuffers[i].mDataByteSize);
            }
            return noErr;
        }
        impl->preBuffering.store(false, std::memory_order_release);
        MS_INFO("CoreAudioBackend: pre-buffer complete (%u frames buffered), starting output", avail);
    }

    float vol = impl->volume.load(std::memory_order_acquire);
    bool applyVolume = vol < 0.999f;
    bool applyFadeOut = impl->fadeOutRemaining.load(std::memory_order_acquire) > 0;
    uint32_t channelCount = impl->channels.load(std::memory_order_acquire);
    float fadeOutGain = impl->fadeOutGain.load(std::memory_order_acquire);

    for (UInt32 bufIdx = 0; bufIdx < ioData->mNumberBuffers; bufIdx++) {
        auto& outBuf = ioData->mBuffers[bufIdx];
        auto* outPtr = static_cast<float*>(outBuf.mData);
        uint32_t framesNeeded = inNumberFrames;
        uint32_t available = impl->ringFramesAvailable();

        if (available >= framesNeeded) {
            impl->ringRead(outPtr, framesNeeded);
        } else if (available > 0) {
            impl->ringRead(outPtr, available);
            uint32_t remaining = framesNeeded - available;
            memset(outPtr + available * channelCount, 0, static_cast<size_t>(remaining) * channelCount * sizeof(float));
            if (impl->fadeOutRemaining.load(std::memory_order_acquire) == 0) {
                impl->underrunCount.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            memset(outPtr, 0, outBuf.mDataByteSize);
            if (impl->fadeOutRemaining.load(std::memory_order_acquire) == 0) {
                impl->underrunCount.fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (applyFadeOut && bufIdx == 0) {
            uint32_t remaining = impl->fadeOutRemaining.load(std::memory_order_acquire);
            uint32_t toFade = std::min(remaining, framesNeeded);
            for (uint32_t f = 0; f < toFade; f++) {
                float t = static_cast<float>(f) / static_cast<float>(FADE_OUT_FRAMES);
                float gain = 1.0f - t * fadeOutGain;
                for (uint32_t c = 0; c < channelCount; c++) {
                    outPtr[f * channelCount + c] *= gain;
                }
            }
            uint32_t newRemaining = remaining - toFade;
            impl->fadeOutRemaining.store(newRemaining, std::memory_order_release);
            if (newRemaining == 0) {
                impl->fadeOutGain.store(1.0f, std::memory_order_release);
            }
        }

        if (applyVolume) {
            uint32_t totalSamples = outBuf.mDataByteSize / sizeof(float);
            for (uint32_t i = 0; i < totalSamples; i++) {
                outPtr[i] *= vol;
            }
        }
    }

    return noErr;
}

CoreAudioBackend::CoreAudioBackend() : m_impl(new Impl()) {}

CoreAudioBackend::~CoreAudioBackend() {
    shutdown();
    delete m_impl;
}

bool CoreAudioBackend::init() {
    m_impl->active.store(true, std::memory_order_release);
    m_impl->playing.store(false, std::memory_order_release);
    m_impl->preBuffering.store(false, std::memory_order_release);
    m_impl->volume.store(1.0f, std::memory_order_release);
    m_impl->frequencyRatio.store(1.0f, std::memory_order_release);
    m_impl->channels.store(2, std::memory_order_release);
    m_impl->sampleRate.store(44100, std::memory_order_release);
    m_impl->bytesPerSample.store(2, std::memory_order_release);
    m_impl->formatFrozen.store(false, std::memory_order_release);
    m_impl->writePos.store(0, std::memory_order_release);
    m_impl->readPos.store(0, std::memory_order_release);
    m_impl->queuedCount.store(0, std::memory_order_release);
    m_impl->underrunCount.store(0, std::memory_order_release);
    m_impl->fadeOutRemaining.store(0, std::memory_order_release);
    m_impl->fadeOutGain.store(1.0f, std::memory_order_release);
    memset(m_impl->ringBuffer, 0, RING_BUFFER_BYTES);

    XAudio2WaveFormat defaultFmt = {};
    defaultFmt.formatTag = 1;
    defaultFmt.channels = 2;
    defaultFmt.samplesPerSec = 44100;
    defaultFmt.bitsPerSample = 16;
    defaultFmt.blockAlign = 4;
    defaultFmt.avgBytesPerSec = 44100 * 4;
    m_impl->format = defaultFmt;

    AudioComponentDescription desc = {};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (!component) {
        MS_WARN("CoreAudioBackend: failed to find default audio output component");
        return true;
    }

    AudioUnit audioUnit = nullptr;
    OSStatus status = AudioComponentInstanceNew(component, &audioUnit);
    if (status != noErr || !audioUnit) {
        MS_WARN("CoreAudioBackend: failed to create AudioUnit (%d)", (int)status);
        return true;
    }

    m_impl->audioUnit = audioUnit;

    AudioStreamBasicDescription asbd = {};
    asbd.mSampleRate = 44100.0;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mBytesPerPacket = 8;
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = 8;
    asbd.mChannelsPerFrame = 2;
    asbd.mBitsPerChannel = 32;
    m_impl->asbd = asbd;

    status =
        AudioUnitSetProperty(audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &asbd, sizeof(asbd));
    if (status != noErr) {
        MS_WARN("CoreAudioBackend: failed to set stream format (%d)", (int)status);
    }

    uint32_t bufFrames = IO_BUFFER_FRAMES;
    status = AudioUnitSetProperty(audioUnit, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &bufFrames,
                                  sizeof(bufFrames));
    if (status != noErr) {
        MS_WARN("CoreAudioBackend: failed to set IO buffer size to %u frames (%d) — using default", bufFrames,
                (int)status);
    } else {
        MS_INFO("CoreAudioBackend: IO buffer set to %u frames (%.1f ms)", bufFrames, bufFrames * 1000.0 / 44100.0);
    }

    AURenderCallbackStruct callback = {};
    callback.inputProc = audioRenderCallback;
    callback.inputProcRefCon = m_impl;

    status = AudioUnitSetProperty(audioUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback,
                                  sizeof(callback));
    if (status != noErr) {
        MS_WARN("CoreAudioBackend: failed to set render callback (%d)", (int)status);
    }

    status = AudioUnitInitialize(audioUnit);
    if (status != noErr) {
        MS_WARN("CoreAudioBackend: failed to initialize AudioUnit (%d)", (int)status);
    }

    MS_INFO("CoreAudioBackend: AudioUnit initialized (44100 Hz, 32-bit float, stereo, lock-free ring buffer)");
    return true;
}

void CoreAudioBackend::shutdown() {
    if (m_impl->playing.load(std::memory_order_acquire))
        stop();

    if (m_impl->audioUnit) {
        AudioOutputUnitStop(m_impl->audioUnit);
        AudioUnitUninitialize(m_impl->audioUnit);
        AudioComponentInstanceDispose(m_impl->audioUnit);
        m_impl->audioUnit = nullptr;
    }

    m_impl->active.store(false, std::memory_order_release);
    m_impl->writePos.store(0, std::memory_order_release);
    m_impl->readPos.store(0, std::memory_order_release);
    m_impl->queuedCount.store(0, std::memory_order_release);
}

bool CoreAudioBackend::isActive() const {
    return m_impl->active.load(std::memory_order_acquire);
}

bool CoreAudioBackend::submitBuffer(const void* data, uint32_t size, const XAudio2WaveFormat& format) {
    if (!data || size == 0)
        return false;

    if (!m_impl->formatFrozen.load(std::memory_order_acquire)) {
        m_impl->format = format;
        m_impl->channels.store(format.channels, std::memory_order_release);
        m_impl->sampleRate.store(format.samplesPerSec, std::memory_order_release);
        m_impl->bytesPerSample.store(format.bitsPerSample == 8    ? 1
                                     : format.bitsPerSample == 16 ? 2
                                     : format.bitsPerSample == 24 ? 3
                                     : format.bitsPerSample == 32 ? 4
                                                                  : 2,
                                     std::memory_order_release);
        m_impl->formatFrozen.store(true, std::memory_order_release);
    }

    uint32_t bytesPerSample = m_impl->bytesPerSample.load(std::memory_order_acquire);
    uint32_t channelCount = m_impl->channels.load(std::memory_order_acquire);
    uint32_t totalSamples = size / bytesPerSample;
    uint32_t totalFloatSamples = totalSamples;
    uint32_t frames = totalSamples / channelCount;

    uint32_t freeFrames = m_impl->ringFramesFree();
    if (frames > freeFrames) {
        frames = freeFrames;
        totalFloatSamples = frames * channelCount;
        MS_TRACE("CoreAudioBackend: ring buffer near-full, dropping %u frames",
                 (size / bytesPerSample / channelCount) - frames);
    }

    if (frames == 0)
        return true;

    float floatBuf[RING_BUFFER_FRAMES * RING_BUFFER_CHANNELS];

    const uint8_t* src = static_cast<const uint8_t*>(data);

    switch (bytesPerSample) {
    case 1:
        convertUInt8ToFloat(src, floatBuf, totalFloatSamples);
        break;
    case 2:
        convertInt16ToFloat(reinterpret_cast<const int16_t*>(src), floatBuf, totalFloatSamples);
        break;
    case 3:
        convertInt24ToFloat(src, floatBuf, totalFloatSamples);
        break;
    case 4:
        if (m_impl->format.formatTag == 3) {
            convertFloatToFloat(reinterpret_cast<const float*>(src), floatBuf, totalFloatSamples);
        } else {
            convertInt32ToFloat(reinterpret_cast<const int32_t*>(src), floatBuf, totalFloatSamples);
        }
        break;
    default:
        convertInt16ToFloat(reinterpret_cast<const int16_t*>(src), floatBuf, totalFloatSamples);
        break;
    }

    m_impl->ringWrite(floatBuf, frames);
    m_impl->queuedCount.fetch_add(1, std::memory_order_relaxed);

    return true;
}

void CoreAudioBackend::setVolume(float v) {
    m_impl->volume.store(v < 0 ? 0 : (v > 1 ? 1 : v), std::memory_order_release);
    if (m_impl->audioUnit) {
        AudioUnitSetParameter(m_impl->audioUnit, kHALOutputParam_Volume, kAudioUnitScope_Output, 0,
                              v < 0 ? 0 : (v > 1 ? 1 : v), 0);
    }
}

float CoreAudioBackend::volume() const {
    return m_impl->volume.load(std::memory_order_acquire);
}

void CoreAudioBackend::play() {
    m_impl->fadeOutRemaining.store(0, std::memory_order_release);
    m_impl->preBuffering.store(true, std::memory_order_release);
    m_impl->playing.store(true, std::memory_order_release);
    if (m_impl->audioUnit) {
        AudioOutputUnitStart(m_impl->audioUnit);
    }
    MS_TRACE("CoreAudioBackend: play — pre-buffering started");
}

void CoreAudioBackend::stop() {
    if (m_impl->playing.load(std::memory_order_acquire)) {
        m_impl->fadeOutGain.store(1.0f, std::memory_order_release);
        m_impl->fadeOutRemaining.store(FADE_OUT_FRAMES, std::memory_order_release);

        uint32_t waited = 0;
        while (m_impl->fadeOutRemaining.load(std::memory_order_acquire) > 0 && waited < FADE_OUT_FRAMES + 100) {
            usleep(500);
            waited++;
        }

        m_impl->playing.store(false, std::memory_order_release);
    }

    if (m_impl->audioUnit) {
        AudioOutputUnitStop(m_impl->audioUnit);
    }

    m_impl->preBuffering.store(false, std::memory_order_release);
    m_impl->writePos.store(0, std::memory_order_release);
    m_impl->readPos.store(0, std::memory_order_release);
    m_impl->queuedCount.store(0, std::memory_order_release);
}

void CoreAudioBackend::pause() {
    if (m_impl->playing.load(std::memory_order_acquire)) {
        m_impl->fadeOutGain.store(0.5f, std::memory_order_release);
        m_impl->fadeOutRemaining.store(FADE_OUT_FRAMES, std::memory_order_release);

        uint32_t waited = 0;
        while (m_impl->fadeOutRemaining.load(std::memory_order_acquire) > 0 && waited < FADE_OUT_FRAMES + 100) {
            usleep(500);
            waited++;
        }

        m_impl->playing.store(false, std::memory_order_release);
    }

    if (m_impl->audioUnit) {
        AudioOutputUnitStop(m_impl->audioUnit);
    }
}

void CoreAudioBackend::setFrequencyRatio(float ratio) {
    m_impl->frequencyRatio.store(ratio < 0.0f ? 0.0f : ratio, std::memory_order_release);
}

float CoreAudioBackend::frequencyRatio() const {
    return m_impl->frequencyRatio.load(std::memory_order_acquire);
}

uint32_t CoreAudioBackend::queuedBufferCount() const {
    return m_impl->queuedCount.load(std::memory_order_acquire);
}

void CoreAudioBackend::flushBuffers() {
    m_impl->writePos.store(0, std::memory_order_release);
    m_impl->readPos.store(0, std::memory_order_release);
    m_impl->queuedCount.store(0, std::memory_order_release);
}

} // namespace metalsharp
