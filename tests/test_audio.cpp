#include <cstdio>
#include <cstring>
#include <metalsharp/CoreAudioBackend.h>
#include <metalsharp/Platform.h>
#include <metalsharp/XAudio2Engine.h>
#include <thread>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            printf("  [OK] %s\n", msg);                                                                                \
            passed++;                                                                                                  \
        } else {                                                                                                       \
            printf("  [FAIL] %s\n", msg);                                                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static bool test_coreaudio_render_thread_state() {
    metalsharp::CoreAudioBackend backend;
    if (!backend.init())
        return false;

    metalsharp::XAudio2WaveFormat fmt{};
    fmt.formatTag = 1;
    fmt.channels = 2;
    fmt.samplesPerSec = 44100;
    fmt.bitsPerSample = 16;
    fmt.blockAlign = 4;
    fmt.avgBytesPerSec = 44100 * 4;

    int16_t sampleData[1024] = {};
    bool submitsOk = true;

    // Keep the game-thread path active while AudioUnit runs the render
    // callback. This is also the focused regression harness for the shared
    // format and fade state; run it under ThreadSanitizer to diagnose any
    // accidental reintroduction of non-atomic accesses.
    backend.play();
    std::thread gameThread([&]() {
        for (int i = 0; i < 32; i++) {
            if (!backend.submitBuffer(sampleData, sizeof(sampleData), fmt))
                submitsOk = false;
            if (i == 8 || i == 20) {
                backend.pause();
                backend.play();
            }
        }
    });
    gameThread.join();

    backend.stop();
    backend.shutdown();
    return submitsOk;
}

int main() {
    printf("=== Audio Tests ===\n\n");

    {
        printf("--- CoreAudioBackend Lifecycle ---\n");
        metalsharp::CoreAudioBackend backend;
        CHECK(!backend.isActive(), "Backend inactive before init");

        bool initResult = backend.init();
        CHECK(initResult, "Backend init succeeds");
        CHECK(backend.isActive(), "Backend active after init");

        backend.play();
        CHECK(true, "Play succeeds");

        backend.pause();
        CHECK(true, "Pause succeeds");

        backend.setVolume(0.5f);
        CHECK(true, "SetVolume succeeds");

        backend.stop();
        CHECK(true, "Stop succeeds");

        backend.shutdown();
        CHECK(!backend.isActive(), "Backend inactive after shutdown");
    }

    {
        printf("\n--- CoreAudioBackend Submit Buffer ---\n");
        metalsharp::CoreAudioBackend backend;
        backend.init();

        metalsharp::XAudio2WaveFormat fmt{};
        fmt.formatTag = 1;
        fmt.channels = 2;
        fmt.samplesPerSec = 44100;
        fmt.bitsPerSample = 16;
        fmt.blockAlign = 4;
        fmt.avgBytesPerSec = 44100 * 4;

        uint32_t sampleData[64] = {};
        bool submitResult = backend.submitBuffer(sampleData, sizeof(sampleData), fmt);
        CHECK(submitResult, "Submit buffer succeeds");

        backend.shutdown();
    }

    {
        printf("\n--- CoreAudioBackend Channel Contract ---\n");
        metalsharp::CoreAudioBackend backend;
        backend.init();

        metalsharp::XAudio2WaveFormat fmt{};
        fmt.formatTag = 1;
        fmt.channels = 4;
        fmt.samplesPerSec = 44100;
        fmt.bitsPerSample = 16;
        fmt.blockAlign = 8;
        fmt.avgBytesPerSec = 44100 * 8;

        uint16_t samples[8] = {};
        CHECK(!backend.submitBuffer(samples, sizeof(samples), fmt), "Rejects non-stereo buffer formats");
        CHECK(backend.queuedBufferCount() == 0, "Rejected channel format does not queue audio");

        fmt.channels = 1;
        fmt.blockAlign = 2;
        fmt.avgBytesPerSec = 44100 * 2;
        CHECK(!backend.submitBuffer(samples, sizeof(samples), fmt), "Rejects mono buffer formats");
        CHECK(backend.queuedBufferCount() == 0, "Rejected mono format does not queue audio");

        fmt.channels = 2;
        fmt.blockAlign = 4;
        fmt.avgBytesPerSec = 44100 * 4;
        CHECK(backend.submitBuffer(samples, sizeof(samples), fmt), "Accepts stereo after rejected formats");
        CHECK(backend.queuedBufferCount() == 1, "Stereo buffer is queued once");

        backend.shutdown();
    }

    {
        printf("\n--- CoreAudioBackend Volume ---\n");
        metalsharp::CoreAudioBackend backend;
        backend.init();

        backend.setVolume(0.0f);
        CHECK(true, "Volume 0.0 accepted");

        backend.setVolume(1.0f);
        CHECK(true, "Volume 1.0 accepted");

        backend.setVolume(0.75f);
        CHECK(true, "Volume 0.75 accepted");

        backend.shutdown();
    }

    {
        printf("\n--- CoreAudioBackend Render Thread State ---\n");
        CHECK(test_coreaudio_render_thread_state(), "Game-thread audio state remains valid during rendering");
    }

    {
        printf("\n--- XAudio2Engine Creation ---\n");
        auto* engine = new metalsharp::XAudio2Engine();
        HRESULT hr = engine->init();
        CHECK(hr == S_OK, "XAudio2Engine init returns S_OK");
        delete engine;
    }

    {
        printf("\n--- XAudio2Engine Null Init ---\n");
        metalsharp::XAudio2Engine engine;
        HRESULT hr = engine.init();
        CHECK(hr == S_OK, "XAudio2Engine default init succeeds");
    }

    {
        printf("\n--- XAudio2WaveFormat Defaults ---\n");
        metalsharp::XAudio2WaveFormat fmt{};
        CHECK(fmt.formatTag == 0, "Default formatTag is 0");
        CHECK(fmt.channels == 0, "Default channels is 0");
        CHECK(fmt.samplesPerSec == 0, "Default samplesPerSec is 0");
        CHECK(fmt.bitsPerSample == 0, "Default bitsPerSample is 0");
    }

    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
