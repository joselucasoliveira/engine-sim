#include "../include/sample_clock.h"
#include "../include/simulation_fault.h"

#include <cmath>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace engine_sim {
namespace {
long double roundedCycles(long double value) {
    const auto nearest = std::round(value);
    const auto tolerance = 64 * std::numeric_limits<long double>::epsilon() * std::max(1.0L, std::abs(value));
    return std::abs(value - nearest) <= tolerance ? nearest : value;
}
}
SampleClock::SampleClock(std::uint32_t frequency, const std::vector<double> &rest, ClockSource &source, Profile profile)
    : m_frequency(frequency), m_source(source), m_previous(rest), m_current(rest),
      m_next(rest.size()), m_interpolated(rest.size()), m_profile(profile) {
    const auto minimum = profile == Profile::Desktop ? 400u : 4000u;
    const auto maximum = profile == Profile::Desktop ? 400000u : 96000u;
    if (frequency < minimum || frequency > maximum || rest.empty()) {
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
    const auto extraTicks = (m_variable || m_pendingFrequency) && count
        ? (std::uint64_t{400000} * count + NativeRate - 1) / NativeRate + 1
        : (std::uint64_t{m_frequency} * count + m_phase) / NativeRate;
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
    const double fraction = position();
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
        if (m_pendingFrequency) {
            m_frequency = m_pendingFrequency;
            m_pendingFrequency = m_phase = m_variablePhase = 0;
            m_offset = 0; m_variable = false;
        }
        startTick();
    }
    std::uint32_t ticks;
    if (m_pendingFrequency) {
        const long double duration = 1.0L / NativeRate;
        const long double remaining = duration - (1.0L - precisePosition()) / m_frequency;
        const long double tolerance = 64 * std::numeric_limits<long double>::epsilon() * duration;
        ticks = remaining > tolerance ? changeFrequencyWithinInterval(remaining)
            : m_variable ? advanceVariable() : advanceFixed();
    }
    else ticks = m_variable ? advanceVariable() : advanceFixed();
    ++m_counts.frames;
    ++m_counts.nativeFrames;
    m_counts.ticks += ticks;
    *output = static_cast<float>(sample) / 32768.0f;
}

double SampleClock::position() const {
    if (!m_variable) return static_cast<double>(m_phase) / NativeRate;
    return static_cast<double>(precisePosition());
}

long double SampleClock::precisePosition() const {
    if (!m_variable) return static_cast<long double>(m_phase) / NativeRate;
    const auto cycles = roundedCycles(m_offset + static_cast<long double>(m_variablePhase) / NativeRate);
    return cycles - std::floor(cycles);
}

void SampleClock::startTick() {
    m_source.latch();
    m_tickActive = true;
}

void SampleClock::finishTick() {
    if (!m_source.tick(m_next.data())) throw SimulationFault("physics made no progress");
    for (double value : m_next) {
        if (!std::isfinite(value)) throw SimulationFault("nonfinite exhaust endpoint");
    }
    m_previous.swap(m_current);
    m_current.swap(m_next);
    m_tickActive = false;
}

std::uint32_t SampleClock::advanceFixed() {
    const auto total = m_phase + m_frequency;
    const auto ticks = total / NativeRate;
    const auto remainder = total % NativeRate;
    for (std::uint32_t i = 0; i < ticks; ++i) {
        finishTick();
        if (i + 1 < ticks || remainder != 0) {
            startTick();
        }
    }
    m_phase = remainder;
    return ticks;
}

std::uint32_t SampleClock::advanceVariable() {
    const auto total = m_variablePhase + m_frequency;
    const auto next = total % NativeRate;
    const auto oldCycles = roundedCycles(m_offset + static_cast<long double>(m_variablePhase) / NativeRate);
    const auto newCycles = roundedCycles(m_offset + static_cast<long double>(next) / NativeRate);
    const auto ticks = static_cast<std::uint32_t>(total / NativeRate
        + static_cast<int>(std::floor(newCycles)) - static_cast<int>(std::floor(oldCycles)));
    const bool atRightEdge = newCycles == std::floor(newCycles);
    for (std::uint32_t i = 0; i < ticks; ++i) {
        finishTick();
        if (i + 1 < ticks || !atRightEdge) startTick();
    }
    m_variablePhase = next;
    return ticks;
}

std::uint32_t SampleClock::changeFrequencyWithinInterval(long double remaining) {
    finishTick();
    m_frequency = m_pendingFrequency;
    m_pendingFrequency = 0;
    const auto cycles = roundedCycles(remaining * m_frequency);
    const auto ticks = static_cast<std::uint32_t>(std::floor(cycles));
    m_offset = cycles - ticks;
    m_variablePhase = 0;
    m_variable = m_offset != 0;
    m_phase = 0;
    startTick();
    for (std::uint32_t i = 0; i < ticks; ++i) {
        finishTick();
        if (i + 1 < ticks || m_offset != 0) startTick();
    }
    return ticks + 1;
}

void SampleClock::requestDesktopFrequency(std::uint32_t frequency) {
    if (m_profile != Profile::Desktop || frequency < 400 || frequency > 400000) {
        throw std::invalid_argument("invalid private desktop simulation frequency");
    }
    m_pendingFrequency = frequency == m_frequency ? 0 : frequency;
}
}
