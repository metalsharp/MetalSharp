/// @file CoreAudioBackend.h
/// @brief CoreAudio-based audio playback backend for XAudio2 emulation.
///
/// Wraps Apple's CoreAudio/AudioToolbox APIs to provide a low-latency audio output
/// path used by the XAudio2 and DirectSound bridges. Handles buffer submission with
/// format conversion, volume control, pitch shifting via frequency ratio, and play/stop/
/// pause lifecycle management. This is the single native audio sink that all Windows
/// audio API shims converge on.

#pragma once

#include <cstdint>
#include <metalsharp/Platform.h>

namespace metalsharp {

struct XAudio2WaveFormat {
    UINT formatTag;
    UINT channels;
    UINT samplesPerSec;
    UINT avgBytesPerSec;
    UINT blockAlign;
    UINT bitsPerSample;
};

class CoreAudioBackend {
  public:
    CoreAudioBackend();
    ~CoreAudioBackend();

    bool init();
    void shutdown();
    bool isActive() const;

    /// Submit interleaved PCM data for the fixed stereo CoreAudio output.
    /// Non-stereo formats are rejected rather than changing the render layout.
    bool submitBuffer(const void* data, uint32_t size, const XAudio2WaveFormat& format);
    void setVolume(float volume);
    float volume() const;
    void play();
    void stop();
    void pause();
    void setFrequencyRatio(float ratio);
    float frequencyRatio() const;

    uint32_t queuedBufferCount() const;
    void flushBuffers();

    struct Impl;

  private:
    Impl* m_impl;
};

} // namespace metalsharp
