/// @file DirectSoundBackend.cpp
/// @brief DirectSound buffer emulation via CoreAudio ring buffer.
///
/// Implements IDirectSound and IDirectSoundBuffer COM interfaces by writing captured audio into the same CoreAudio ring
/// buffer used by XAudio2. Supports primary/secondary buffer semantics and write cursor tracking.
#include <algorithm>
#include <cstring>
#include <metalsharp/DirectSoundBackend.h>
#include <metalsharp/Logger.h>

namespace metalsharp {

DirectSoundBackend& DirectSoundBackend::instance() {
    static DirectSoundBackend inst;
    return inst;
}

bool DirectSoundBackend::init() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_initialized = true;
    MS_INFO("DirectSoundBackend initialized");
    return true;
}

void DirectSoundBackend::shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto* buf : m_buffers) {
        delete buf;
    }
    m_buffers.clear();
    m_initialized = false;
}

void* DirectSoundBackend::createBuffer(uint32_t size, const WAVEFORMAT& format) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto* buf = new DSBuffer();
    buf->data.resize(size, 0);
    buf->format = format;
    buf->volume = 1.0f;
    buf->playing = false;
    buf->writeCursor = 0;
    m_buffers.push_back(buf);
    MS_TRACE("DirectSoundBackend: buffer created (%u bytes, %u Hz, %u-bit, %u ch)", size, format.nSamplesPerSec,
             format.wBitsPerSample, format.nChannels);
    return buf;
}

void DirectSoundBackend::destroyBuffer(void* buffer) {
    if (!buffer)
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto* buf = static_cast<DSBuffer*>(buffer);
    auto it = std::find(m_buffers.begin(), m_buffers.end(), buf);
    if (it == m_buffers.end())
        return;

    m_buffers.erase(it);
    delete buf;
}

bool DirectSoundBackend::writeBuffer(void* buffer, const void* data, uint32_t offset, uint32_t size) {
    if (!buffer || !data)
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto* buf = static_cast<DSBuffer*>(buffer);
    if (std::find(m_buffers.begin(), m_buffers.end(), buf) == m_buffers.end())
        return false;
    if (offset + size > buf->data.size())
        return false;
    memcpy(buf->data.data() + offset, data, size);
    return true;
}

bool DirectSoundBackend::playBuffer(void* buffer, uint32_t flags) {
    if (!buffer)
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto* buf = static_cast<DSBuffer*>(buffer);
    if (std::find(m_buffers.begin(), m_buffers.end(), buf) == m_buffers.end())
        return false;
    buf->playing = true;
    return true;
}

bool DirectSoundBackend::stopBuffer(void* buffer) {
    if (!buffer)
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto* buf = static_cast<DSBuffer*>(buffer);
    if (std::find(m_buffers.begin(), m_buffers.end(), buf) == m_buffers.end())
        return false;
    buf->playing = false;
    return true;
}

bool DirectSoundBackend::setVolume(void* buffer, float volume) {
    if (!buffer)
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto* buf = static_cast<DSBuffer*>(buffer);
    if (std::find(m_buffers.begin(), m_buffers.end(), buf) == m_buffers.end())
        return false;
    buf->volume = volume;
    return true;
}

float DirectSoundBackend::getVolume(void* buffer) const {
    if (!buffer)
        return 0;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto* buf = static_cast<DSBuffer*>(buffer);
    if (std::find(m_buffers.begin(), m_buffers.end(), buf) == m_buffers.end())
        return 0;
    return buf->volume;
}

} // namespace metalsharp
