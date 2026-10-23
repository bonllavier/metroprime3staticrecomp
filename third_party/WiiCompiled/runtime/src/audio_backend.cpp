#include "audio_backend.h"

#include "runtime_log.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include <SDL3/SDL_init.h>

AudioBackend& AudioBackend::Instance() {
    static AudioBackend instance;
    return instance;
}

float AudioBackend::EffectiveGainLocked() const {
    return m_muted ? 0.0f : m_masterVolume;
}

void AudioBackend::ApplyGainLocked() {
    if (m_stream && !SDL_SetAudioStreamGain(m_stream, EffectiveGainLocked())) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_SetAudioStreamGain failed: " << SDL_GetError() << std::endl;
    }
}

void AudioBackend::SetMasterVolume(float volume) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_masterVolume = std::clamp(volume, 0.0f, 1.0f);
    ApplyGainLocked();
}

void AudioBackend::SetMuted(bool muted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_muted = muted;
    ApplyGainLocked();
}

bool AudioBackend::EnsureInitializedLocked(uint32_t sampleRate, uint32_t channels) {
    if (m_initialized && m_sampleRate == sampleRate && m_channels == channels) {
        return true;
    }

    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_InitSubSystem(SDL_INIT_AUDIO) failed: " << SDL_GetError() << std::endl;
        return false;
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = static_cast<int>(channels);
    spec.freq = static_cast<int>(sampleRate);

    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_OpenAudioDeviceStream failed: " << SDL_GetError() << std::endl;
        return false;
    }

    if (!SDL_ResumeAudioStreamDevice(stream)) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_ResumeAudioStreamDevice failed: " << SDL_GetError() << std::endl;
        SDL_DestroyAudioStream(stream);
        return false;
    }

    if (!SDL_SetAudioStreamGain(stream, EffectiveGainLocked())) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_SetAudioStreamGain failed: " << SDL_GetError() << std::endl;
        SDL_DestroyAudioStream(stream);
        return false;
    }

    m_stream = stream;
    m_spec = spec;
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_initialized = true;
    SDL_AudioSpec deviceSpec{};
    int deviceFrames = 0;
    SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &deviceSpec, &deviceFrames);
    RT_LOG(RT_TAG_AUDIO) << "[audio-backend] playback stream open rate=" << sampleRate
                         << " channels=" << channels << " gain=" << EffectiveGainLocked()
                         << " driver=" << SDL_GetCurrentAudioDriver()
                         << " device=" << SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream))
                         << " output_rate=" << deviceSpec.freq << " output_channels=" << deviceSpec.channels
                         << " output_format=" << deviceSpec.format << " device_frames=" << deviceFrames
                         << std::endl;
    return true;
}

bool AudioBackend::Init(uint32_t sampleRate, uint32_t channels) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return EnsureInitializedLocked(sampleRate, channels);
}

void AudioBackend::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }
    if (m_initialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    m_initialized = false;
    m_sampleRate = 0;
    m_channels = 0;
    m_reportedDroppedBlock = false;
    m_convertBuffer.clear();
}

uint32_t AudioBackend::QueueLimitBytesLocked() const {
    constexpr uint32_t queueMs = 120;

    const uint64_t bytesPerSecond = static_cast<uint64_t>(m_spec.freq) *
                                    static_cast<uint64_t>(m_spec.channels) *
                                    sizeof(int16_t);
    return static_cast<uint32_t>((bytesPerSecond * queueMs) / 1000u);
}

bool AudioBackend::QueueHasCapacityLocked(int incomingBytes) {
    if (!m_stream) {
        return false;
    }
    const int queued = SDL_GetAudioStreamQueued(m_stream);
    if (queued < 0) {
        return true;
    }
    const uint32_t maxQueued = QueueLimitBytesLocked();
    if (static_cast<uint64_t>(queued) + static_cast<uint64_t>(std::max(incomingBytes, 0)) > maxQueued) {
        // Match Dolphin's FIFO overflow behavior: preserve the continuous audio
        // already queued and discard the new block.  Clearing SDL's entire
        // stream creates an audible discontinuity (the severe crackle seen when
        // VI-batched DMA briefly outran playback).
        // A drop is audible; report the first one so it is not invisible.
        if (!m_reportedDroppedBlock) {
            m_reportedDroppedBlock = true;
            RT_LOG(RT_TAG_AUDIO) << "output queue full (" << queued << "/" << maxQueued
                      << " bytes); dropping blocks to preserve continuity" << std::endl;
        }
        return false;
    }
    return true;
}

bool AudioBackend::PushWiiAiSamplesBE16(const uint8_t* data, size_t bytes) {
    if (!data || bytes == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || !m_stream) {
        return false;
    }

    const size_t sampleCount = bytes / sizeof(int16_t);
    if (sampleCount == 0) {
        return false;
    }

    if (m_convertBuffer.size() < sampleCount) {
        m_convertBuffer.resize(sampleCount);
    }

    const size_t frameCount = sampleCount / 2;
    for (size_t frame = 0; frame < frameCount; ++frame) {
        const size_t rightOffset = frame * 4;
        const size_t leftOffset = rightOffset + 2;
        const uint16_t right = static_cast<uint16_t>(data[rightOffset]) << 8 |
                               static_cast<uint16_t>(data[rightOffset + 1]);
        const uint16_t left = static_cast<uint16_t>(data[leftOffset]) << 8 |
                              static_cast<uint16_t>(data[leftOffset + 1]);
        m_convertBuffer[frame * 2] = static_cast<int16_t>(left);
        m_convertBuffer[frame * 2 + 1] = static_cast<int16_t>(right);
    }

    const int lenBytes = static_cast<int>(frameCount * 2 * sizeof(int16_t));
    if (!QueueHasCapacityLocked(lenBytes)) {
        return true;
    }
    if (!SDL_PutAudioStreamData(m_stream, m_convertBuffer.data(), lenBytes)) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_PutAudioStreamData failed: " << SDL_GetError() << std::endl;
        return false;
    }

    static std::atomic<bool> loggedFirstWiiBlock{false};
    if (!loggedFirstWiiBlock.exchange(true, std::memory_order_relaxed)) {
        RT_LOG(RT_TAG_AUDIO) << "[audio-backend] first Wii BE PCM queued bytes=" << lenBytes
                             << " queued=" << SDL_GetAudioStreamQueued(m_stream)
                             << " gain=" << EffectiveGainLocked() << std::endl;
    }

    static std::atomic<bool> loggedFirstNonzeroWiiBlock{false};
    if (!loggedFirstNonzeroWiiBlock.load(std::memory_order_relaxed)) {
        int peak = 0;
        for (size_t i = 0; i < frameCount * 2; ++i)
            peak = std::max(peak, std::abs(static_cast<int>(m_convertBuffer[i])));
        if (peak != 0 && !loggedFirstNonzeroWiiBlock.exchange(true, std::memory_order_relaxed))
            RT_LOG(RT_TAG_AUDIO) << "[audio-backend] first nonzero Wii PCM peak=" << peak
                << " bytes=" << lenBytes << " queued=" << SDL_GetAudioStreamQueued(m_stream)
                << " gain=" << EffectiveGainLocked() << std::endl;
    }

    // Bounded startup diagnostics distinguish a live silent ring from lost host
    // output. Never synthesize samples or alter gain to make the meter move.
    static uint64_t diagnosticFrames = 0;
    static int diagnosticPeak = 0;
    static unsigned diagnosticReports = 0;
    if (diagnosticReports < 4u && loggedFirstNonzeroWiiBlock.load(std::memory_order_relaxed)) {
        diagnosticFrames += frameCount;
        for (size_t i = 0; i < frameCount * 2; ++i)
            diagnosticPeak = std::max(diagnosticPeak, std::abs(static_cast<int>(m_convertBuffer[i])));
        if (diagnosticFrames >= m_sampleRate) {
            RT_LOG(RT_TAG_AUDIO) << "[audio-backend] Wii PCM second=" << ++diagnosticReports
                << " peak=" << diagnosticPeak << " queued=" << SDL_GetAudioStreamQueued(m_stream)
                << " paused=" << SDL_AudioDevicePaused(SDL_GetAudioStreamDevice(m_stream))
                << " gain=" << EffectiveGainLocked() << std::endl;
            diagnosticFrames = 0;
            diagnosticPeak = 0;
        }
    }

    return true;
}

bool AudioBackend::PushSamplesLE16(const int16_t* samples, size_t sampleCount) {
    if (!samples || sampleCount == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || !m_stream) {
        return false;
    }

    const int lenBytes = static_cast<int>(sampleCount * sizeof(int16_t));
    if (!QueueHasCapacityLocked(lenBytes)) {
        return true;
    }
    if (!SDL_PutAudioStreamData(m_stream, samples, lenBytes)) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_PutAudioStreamData failed: " << SDL_GetError() << std::endl;
        return false;
    }

    static std::atomic<bool> loggedFirstSamples{false};
    if (!loggedFirstSamples.exchange(true, std::memory_order_relaxed)) {
        RT_LOG(RT_TAG_AUDIO) << "[audio-backend] first PCM queued bytes=" << lenBytes
                             << " queued=" << SDL_GetAudioStreamQueued(m_stream)
                             << " gain=" << EffectiveGainLocked() << std::endl;
    }

    return true;
}
