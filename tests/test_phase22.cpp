#include <cmath>
#include <cstdio>
#include <cstring>
#include <metalsharp/CoreAudioBackend.h>
#include <metalsharp/DirectSoundBackend.h>
#include <metalsharp/X3DAudioEngine.h>
#include <metalsharp/XAudio2Engine.h>

using namespace metalsharp;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name)                                                                                                     \
    printf("  TEST: %-55s", #name);                                                                                    \
    if (test_##name()) {                                                                                               \
        printf("PASS\n");                                                                                              \
        testsPassed++;                                                                                                 \
    } else {                                                                                                           \
        printf("FAIL\n");                                                                                              \
        testsFailed++;                                                                                                 \
    }

// 22.1 CoreAudio / XAudio2
static bool test_coreaudio_lifecycle() {
    CoreAudioBackend backend;
    if (backend.isActive())
        return false;

    bool initOk = backend.init();
    if (!initOk)
        return false;
    if (!backend.isActive())
        return false;

    backend.play();
    backend.pause();
    backend.stop();
    backend.shutdown();
    return !backend.isActive();
}

static bool test_coreaudio_submit_buffer() {
    CoreAudioBackend backend;
    backend.init();

    XAudio2WaveFormat fmt{};
    fmt.formatTag = 1;
    fmt.channels = 2;
    fmt.samplesPerSec = 44100;
    fmt.bitsPerSample = 16;
    fmt.blockAlign = 4;
    fmt.avgBytesPerSec = 44100 * 4;

    uint32_t samples[256] = {};
    bool submitOk = backend.submitBuffer(samples, sizeof(samples), fmt);
    backend.shutdown();
    return submitOk;
}

static bool test_coreaudio_volume() {
    CoreAudioBackend backend;
    backend.init();

    backend.setVolume(0.0f);
    if (backend.volume() != 0.0f)
        return false;

    backend.setVolume(1.0f);
    if (backend.volume() != 1.0f)
        return false;

    backend.setVolume(1.5f);
    if (backend.volume() != 1.0f)
        return false;

    backend.setVolume(-0.5f);
    if (backend.volume() != 0.0f)
        return false;

    backend.shutdown();
    return true;
}

static bool test_coreaudio_frequency_ratio() {
    CoreAudioBackend backend;
    backend.init();

    backend.setFrequencyRatio(2.0f);
    bool ok = backend.frequencyRatio() == 2.0f;
    backend.shutdown();
    return ok;
}

static bool test_coreaudio_queued_buffers() {
    CoreAudioBackend backend;
    backend.init();

    XAudio2WaveFormat fmt = {1, 2, 44100, 176400, 4, 16};
    uint32_t samples[64] = {};

    if (backend.queuedBufferCount() != 0)
        return false;
    backend.submitBuffer(samples, sizeof(samples), fmt);
    backend.submitBuffer(samples, sizeof(samples), fmt);
    if (backend.queuedBufferCount() != 2)
        return false;

    backend.flushBuffers();
    bool empty = backend.queuedBufferCount() == 0;
    backend.shutdown();
    return empty;
}

static bool test_xaudio2_create() {
    auto* engine = new XAudio2Engine();
    HRESULT hr = engine->init();
    bool ok = hr == S_OK;
    delete engine;
    return ok;
}

static bool test_xaudio2_default_init() {
    XAudio2Engine engine;
    HRESULT hr = engine.init();
    return hr == S_OK;
}

static bool test_xaudio2_source_voice() {
    XAudio2Engine engine;
    engine.init();

    void* voice = nullptr;
    XAudio2WaveFormat fmt = {1, 2, 44100, 176400, 4, 16};
    HRESULT hr = engine.createSourceVoice(&voice, &fmt);
    return hr == S_OK && voice != nullptr;
}

static bool test_xaudio2_submit_start_stop() {
    XAudio2Engine engine;
    engine.init();

    void* voice = nullptr;
    XAudio2WaveFormat fmt = {1, 2, 44100, 176400, 4, 16};
    engine.createSourceVoice(&voice, &fmt);

    struct {
        uint32_t flags;
        uint32_t audioBytes;
        const void* pAudioData;
        uint32_t pb, pl, lb, ll, lc;
        void* ctx;
    } buf = {};
    uint32_t samples[64] = {};
    buf.audioBytes = sizeof(samples);
    buf.pAudioData = samples;

    HRESULT submit = engine.submitSourceBuffer(voice, &buf);
    HRESULT start = engine.start(voice);
    HRESULT stop = engine.stop(voice);
    return submit == S_OK && start == S_OK && stop == S_OK;
}

// 22.2 X3DAudio
static bool test_x3daudio_init() {
    auto& engine = X3DAudioEngine::instance();
    engine.init(0x3);
    engine.shutdown();
    return true;
}

static bool test_x3daudio_distance_attenuation() {
    auto& engine = X3DAudioEngine::instance();
    engine.init(0x3);
    engine.setDistanceCurve(1.0f, 100.0f, 1.0f);

    float atNear = engine.computeDistanceAttenuation(0.5f);
    float atMid = engine.computeDistanceAttenuation(50.0f);
    float atFar = engine.computeDistanceAttenuation(200.0f);

    engine.shutdown();
    return atNear == 1.0f && atFar == 0.0f && atMid > 0.0f && atMid < 1.0f;
}

static bool test_x3daudio_pan() {
    auto& engine = X3DAudioEngine::instance();
    engine.init(0x3);

    Audio3DListener listener = {{0, 0, 0}, {0, 0, -1}, {0, 1, 0}, {0, 0, 0}, 1, 1, 0};
    Audio3DEmitter left = {{-5, 0, -5}, {0, 0, -1}, {0, 1, 0}, {0, 0, 0}, 0, 0, 1, 1, 1, 1};
    Audio3DEmitter right = {{5, 0, -5}, {0, 0, -1}, {0, 1, 0}, {0, 0, 0}, 0, 0, 1, 1, 1, 1};

    float panL = engine.computePan(listener, left);
    float panR = engine.computePan(listener, right);

    engine.shutdown();
    return panL < panR;
}

static bool test_x3daudio_doppler() {
    auto& engine = X3DAudioEngine::instance();
    engine.init(0x3);
    engine.setDopplerFactor(1.0f);

    Audio3DListener listener = {{0, 0, 0}, {0, 0, -1}, {0, 1, 0}, {0, 0, 0}, 1, 1, 0};
    Audio3DEmitter approaching = {{0, 0, -10}, {0, 0, -1}, {0, 1, 0}, {0, 0, 50}, 0, 0, 1, 1, 1, 1};

    float doppler = engine.computeDoppler(listener, approaching);
    engine.shutdown();
    return doppler > 0;
}

static bool test_x3daudio_calculate() {
    auto& engine = X3DAudioEngine::instance();
    engine.init(0x3);

    Audio3DListener listener = {{0, 0, 0}, {0, 0, -1}, {0, 1, 0}, {0, 0, 0}, 1, 1, 0};
    Audio3DEmitter emitter = {{0, 0, -10}, {0, 0, -1}, {0, 1, 0}, {0, 0, 0}, 0, 0, 1, 1, 1, 1};

    Audio3DOutput output = {};
    engine.calculate(listener, emitter, 0, 2, output);

    engine.shutdown();
    return output.emitterToListenerDistance > 0 && output.dopplerFactor > 0;
}

static bool test_x3daudio_surround_matrix_bounds() {
    auto& engine = X3DAudioEngine::instance();
    engine.init(0x3);

    Audio3DListener listener = {{0, 0, 0}, {0, 0, -1}, {0, 1, 0}, {0, 0, 0}, 1, 1, 0};
    Audio3DEmitter emitter = {{0, 0, -10}, {0, 0, -1}, {0, 1, 0}, {0, 0, 0}, 0, 0, 18, 1, 1, 1};

    struct GuardedOutput {
        Audio3DOutput output;
        uint32_t guard[32];
    } guarded = {};
    for (auto& value : guarded.guard)
        value = 0xA5A5A5A5;

    engine.calculate(listener, emitter, 0, 8, guarded.output);

    bool guardIntact = true;
    for (auto value : guarded.guard) {
        if (value != 0xA5A5A5A5) {
            guardIntact = false;
            break;
        }
    }

    bool matrixInitialized = true;
    for (float coefficient : guarded.output.matrixCoefficients) {
        if (!std::isfinite(coefficient) || coefficient <= 0.0f) {
            matrixInitialized = false;
            break;
        }
    }

    engine.shutdown();
    return guardIntact && matrixInitialized;
}

// 22.3 DirectSound
static bool test_directsound_init() {
    auto& ds = DirectSoundBackend::instance();
    bool ok = ds.init();
    ds.shutdown();
    return ok;
}

static bool test_directsound_create_buffer() {
    auto& ds = DirectSoundBackend::instance();
    ds.init();

    WAVEFORMAT fmt = {1, 2, 44100, 176400, 4, 16, 0};
    void* buf = ds.createBuffer(4096, fmt);
    bool ok = buf != nullptr;

    ds.destroyBuffer(buf);
    ds.shutdown();
    return ok;
}

static bool test_directsound_write_play_stop() {
    auto& ds = DirectSoundBackend::instance();
    ds.init();

    WAVEFORMAT fmt = {1, 2, 44100, 176400, 4, 16, 0};
    void* buf = ds.createBuffer(1024, fmt);

    uint8_t data[512] = {};
    bool writeOk = ds.writeBuffer(buf, data, 0, 512);
    bool playOk = ds.playBuffer(buf, 0);
    bool stopOk = ds.stopBuffer(buf);

    ds.destroyBuffer(buf);
    ds.shutdown();
    return writeOk && playOk && stopOk;
}

static bool test_directsound_volume() {
    auto& ds = DirectSoundBackend::instance();
    ds.init();

    WAVEFORMAT fmt = {1, 2, 44100, 176400, 4, 16, 0};
    void* buf = ds.createBuffer(1024, fmt);

    ds.setVolume(buf, 0.5f);
    float vol = ds.getVolume(buf);

    ds.destroyBuffer(buf);
    ds.shutdown();
    return std::abs(vol - 0.5f) < 0.01f;
}

int main() {
    printf("=== Phase 22: Audio Pipeline ===\n\n");

    printf("--- 22.1 XAudio2 / CoreAudio Backend ---\n");
    TEST(coreaudio_lifecycle);
    TEST(coreaudio_submit_buffer);
    TEST(coreaudio_volume);
    TEST(coreaudio_frequency_ratio);
    TEST(coreaudio_queued_buffers);
    TEST(xaudio2_create);
    TEST(xaudio2_default_init);
    TEST(xaudio2_source_voice);
    TEST(xaudio2_submit_start_stop);

    printf("\n--- 22.2 X3DAudio Positional Audio ---\n");
    TEST(x3daudio_init);
    TEST(x3daudio_distance_attenuation);
    TEST(x3daudio_pan);
    TEST(x3daudio_doppler);
    TEST(x3daudio_calculate);
    TEST(x3daudio_surround_matrix_bounds);

    printf("\n--- 22.3 DirectSound Fallback ---\n");
    TEST(directsound_init);
    TEST(directsound_create_buffer);
    TEST(directsound_write_play_stop);
    TEST(directsound_volume);

    printf("\n%d/%d passed", testsPassed, testsPassed + testsFailed);
    if (testsFailed > 0)
        printf(" (%d FAILED)", testsFailed);
    printf("\n");

    return testsFailed > 0 ? 1 : 0;
}
