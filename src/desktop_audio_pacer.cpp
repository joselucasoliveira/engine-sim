#include "desktop_audio_pacer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace desktop {
AudioPacer::AudioPacer(std::function<float()> source) : m_source(std::move(source)) {
    if (!m_source) throw DesktopError("source", "a pull renderer is required");
}

AudioProgress AudioPacer::progress(Host &device) {
    auto value = device.audioProgress();
    if (value.cursor >= Rate || value.queued > Rate || value.writable > Rate) {
        throw DesktopError("audio progress", "invalid bounded device snapshot");
    }
    if (!m_haveDevice) {
        m_previousCursor = value.cursor;
        m_fallbackConsumed = value.cursor;
        m_fallbackWritten = m_fallbackConsumed + Target;
        m_writeCursor = (value.cursor + Target) % Rate;
        m_haveDevice = true;
        m_stats.epoch = value.epoch;
    }
    if (value.epoch != m_stats.epoch) throw DesktopError("audio progress", "transport epoch changed without a new source owner");
    if (!value.queueKnown) {
        m_fallbackConsumed += (value.cursor + Rate - m_previousCursor) % Rate;
        m_previousCursor = value.cursor;
        if (m_fallbackConsumed > m_fallbackWritten) {
            m_stats.deviceUnderruns += m_fallbackConsumed - m_fallbackWritten;
            m_fallbackWritten = m_fallbackConsumed;
            m_writeCursor = value.cursor;
        }
        value.queued = static_cast<AudioOffset>(m_fallbackWritten - m_fallbackConsumed);
        value.writable = Rate - value.queued;
        value.consumed = m_fallbackConsumed;
    }
    if (value.writeOffsetKnown) {
        if (value.writeOffset >= Rate) throw DesktopError("audio progress", "invalid device write offset");
        m_writeCursor = value.writeOffset;
    }
    m_stats.consumed = value.consumed;
    m_stats.consumedSource = value.consumedSource;
    m_stats.discarded = value.discardedSource;
    m_stats.starvationSilence = value.starvationSilence;
    m_stats.pauseSilence = value.pauseSilence;
    m_stats.primingSilence = value.primingSilence;
    return value;
}

void AudioPacer::submit(Host &device, AudioProgress &state) {
    const auto requested = std::min<std::size_t>(m_count, state.writable);
    if (requested == 0) return;
    const auto segments = device.lockAudio(m_writeCursor, static_cast<AudioOffset>(requested));
    const std::size_t accepted = static_cast<std::size_t>(segments.firstCount) + segments.secondCount;
    if (accepted > requested || (segments.firstCount && !segments.first) || (segments.secondCount && !segments.second)) {
        throw DesktopError("audio submission", "invalid partial device span");
    }
    for (std::size_t i = 0; i < accepted; ++i) {
        const auto sample = m_ready[(m_begin + i) % Capacity];
        if (i < segments.firstCount) segments.first[i] = sample;
        else segments.second[i - segments.firstCount] = sample;
    }
    device.unlockAudio(segments);
    m_begin = (m_begin + accepted) % Capacity;
    m_count -= accepted;
    m_stats.submitted += accepted;
    state.queued += static_cast<AudioOffset>(accepted);
    state.writable -= static_cast<AudioOffset>(accepted);
    m_writeCursor = (m_writeCursor + static_cast<AudioOffset>(accepted)) % Rate;
    if (!state.queueKnown) m_fallbackWritten += accepted;
}

float AudioPacer::sourceFrame() {
    ++m_stats.requestedSource;
    const float value = m_source();
    if (!std::isfinite(value) || value < -1 || value > 32767.0f / 32768.0f) {
        throw DesktopError("pull source", "renderer returned an invalid mono frame");
    }
    ++m_stats.generatedSource;
    return value;
}

std::int16_t AudioPacer::deviceFrame(unsigned divisor) {
    if (!m_haveSource) {
        m_previous = m_current = sourceFrame();
        m_haveSource = true;
    }
    else if (m_nextSource) {
        const float next = sourceFrame();
        m_previous = m_current;
        m_current = next;
        m_nextSource = false;
    }
    if (divisor != 1) m_shifted = true;
    const double fraction = static_cast<double>(m_phase) / 1000;
    const double sample = !m_shifted ? m_current
        : (1 - fraction) * m_previous + fraction * m_current;
    m_phase += 1000 / divisor;
    if (m_phase >= 1000) { m_phase -= 1000; m_nextSource = true; }
    const auto quantized = std::lround(sample * 32768.0);
    return static_cast<std::int16_t>(std::max<long>(-32768, std::min<long>(32767, quantized)));
}

void AudioPacer::produce(std::uint32_t count, unsigned divisor) {
    m_stats.requestedDevice += count;
    m_stats.reserved = count;
    const auto first = (m_begin + m_count) % Capacity;
    std::size_t produced = 0;
    try {
        while (produced < count) {
            const auto sample = deviceFrame(divisor);
            m_ready[(m_begin + m_count) % Capacity] = sample;
            ++m_count; ++produced; ++m_stats.generatedDevice; --m_stats.reserved;
        }
    }
    catch (const std::exception &error) {
        m_faulted = true; ++m_stats.sourceFaults;
        std::snprintf(m_error.data(), m_error.size(), "%s", error.what());
    }
    catch (...) {
        m_faulted = true; ++m_stats.sourceFaults;
        std::snprintf(m_error.data(), m_error.size(), "unknown pull source failure");
    }
    m_stats.reserved = 0;
    if (m_observation.ready && produced) {
        const auto a = std::min(produced, Capacity - first);
        m_observation.ready(m_observation.context, m_ready.data() + first, a);
        if (produced > a) m_observation.ready(m_observation.context, m_ready.data(), produced - a);
    }
}

bool AudioPacer::service(Host &device, unsigned divisor, bool paused, std::uint32_t manual) {
    if (divisor != 1 && divisor != 10 && divisor != 100 && divisor != 200 && divisor != 500 && divisor != 1000) {
        throw DesktopError("audio speed", "unsupported transport ratio");
    }
    if (manual > Target) throw DesktopError("manual step", "quantum exceeds bounded transport work");
    device.setAudioSourcePaused(paused);
    auto state = progress(device);
    submit(device, state);
    const auto total = static_cast<std::uint64_t>(state.queued) + m_count;
    const auto room = state.writable > m_count ? state.writable - m_count : 0;
    std::uint32_t wanted = 0;
    if (paused && manual) {
        if (total + manual <= 2 * Target && manual <= Capacity - m_count && manual <= room) wanted = manual;
        else ++m_stats.rejectedManualSteps;
    }
    else if (!paused && total < Target) wanted = static_cast<std::uint32_t>(Target - total);
    wanted = static_cast<std::uint32_t>(std::min<std::size_t>({wanted, Capacity - m_count, room}));
    if (!m_faulted && wanted) produce(wanted, divisor);
    submit(device, state);
    m_stats.ready = m_count;
    m_stats.deviceQueued = state.queued;
    m_stats.maximumQueued = std::max(m_stats.maximumQueued, static_cast<std::uint64_t>(state.queued) + m_count);
    return !m_faulted;
}
}
