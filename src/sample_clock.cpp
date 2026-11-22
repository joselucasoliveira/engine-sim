#include "../include/sample_clock.h"
#include "../include/simulation_fault.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace engine_sim {
SampleClock::SampleClock(std::uint32_t frequency, const std::vector<double> &rest, ClockSource &source)
    : m_frequency(frequency), m_source(source), m_previous(rest), m_current(rest),
      m_next(rest.size()), m_interpolated(rest.size()) {
    if (frequency < 4000 || frequency > 96000 || rest.empty()) {
        throw std::invalid_argument("sample clock requires a supported frequency and exhaust channels");
    }
    for (double value : rest) {
        if (!std::isfinite(value)) throw std::invalid_argument("nonfinite prepared exhaust input");
    }
}

void SampleClock::validateSpan(float *output, std::size_t count) const {
    if (m_faulted) throw SimulationFault("sample clock requires recovery after a fault");
    if (count > MaximumBlock || (count != 0 && output == nullptr)) {
        throw std::invalid_argument("invalid sample clock output span");
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    // Only the bounded request is multiplied. No lifetime frame product exists.
    const auto extraTicks = (std::uint64_t{m_frequency} * count + m_phase) / NativeRate;
    if (count > maximum - m_counts.frames || count > maximum - m_counts.nativeFrames
        || extraTicks > maximum - m_counts.ticks) {
        throw std::overflow_error("sample clock counter capacity exceeded");
    }
}

void SampleClock::render(float *output, std::size_t count) {
    m_written = 0;
    validateSpan(output, count);
    try {
        while (m_written < count) {
            interval(output + m_written);
            ++m_written;
        }
    }
    catch (...) {
        m_faulted = true;
        throw;
    }
}

void SampleClock::interval(float *output) {
    const double fraction = static_cast<double>(m_phase) / NativeRate;
    for (std::size_t i = 0; i < m_current.size(); ++i) {
        const double value = (1 - fraction) * m_previous[i] + fraction * m_current[i];
        if (!std::isfinite(value)) throw SimulationFault("nonfinite interpolated exhaust input");
        m_interpolated[i] = value;
    }
    std::int16_t sample;
    if (!m_source.native(m_interpolated.data(), sample)) {
        throw SimulationFault("native synthesis made no progress");
    }
    if (!m_tickActive) {
        m_source.latch();
        m_tickActive = true;
    }
    const auto total = m_phase + m_frequency;
    const auto ticks = total / NativeRate;
    const auto remainder = total % NativeRate;
    for (std::uint32_t i = 0; i < ticks; ++i) {
        if (!m_source.tick(m_next.data())) throw SimulationFault("physics made no progress");
        for (double value : m_next) {
            if (!std::isfinite(value)) throw SimulationFault("nonfinite exhaust endpoint");
        }
        m_previous.swap(m_current);
        m_current.swap(m_next);
        m_tickActive = false;
        if (i + 1 < ticks || remainder != 0) {
            m_source.latch();
            m_tickActive = true;
        }
    }
    m_phase = remainder;
    ++m_counts.frames;
    ++m_counts.nativeFrames;
    m_counts.ticks += ticks;
    *output = static_cast<float>(sample) / 32768.0f;
}
}
