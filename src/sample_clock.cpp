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
SampleClock::SampleClock(std::uint32_t frequency, const std::vector<double> &rest, ClockSource &source, Profile profile, std::uint32_t outputRate)
    : m_frequency(frequency), m_outputRate(outputRate), m_source(source), m_previous(rest), m_current(rest),
      m_next(rest.size()), m_interpolated(rest.size()), m_profile(profile) {
    const auto minimum = profile == Profile::Desktop ? 400u : 4000u;
    const auto maximum = profile == Profile::Desktop ? 400000u : 96000u;
    if (frequency < minimum || frequency > maximum || rest.empty()
        || (outputRate != NativeRate && outputRate != 48000)
        || (profile == Profile::Desktop && outputRate != NativeRate)) {
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
    const auto ticks = m_outputRate == NativeRate ? extraTicks
        : (std::uint64_t{m_frequency} * count + m_outputTickPhase) / m_outputRate;
    const auto native = m_outputRate == NativeRate ? count
        : (std::uint64_t{NativeRate} * count + m_outputNativePhase + m_outputRate - 1) / m_outputRate - (m_outputNativePhase != 0);
    if (count > maximum - m_counts.frames || native > maximum - m_counts.nativeFrames
        || ticks > maximum - m_counts.ticks) {
        throw std::overflow_error("sample clock counter capacity exceeded");
    }
}

void SampleClock::render(float *output, std::size_t count) {
    m_written = 0;
    validateSpan(output, count);
    try {
        while (m_written < count) {
            if (m_outputRate == NativeRate) interval(output + m_written);
            else convertedInterval(output + m_written);
            ++m_written;
        }
    }
    catch (...) {
        m_faulted = true;
        throw;
    }
}

std::int16_t SampleClock::nativeFrame(double fraction) {
    for (std::size_t i = 0; i < m_current.size(); ++i) {
        const double value = (1 - fraction) * m_previous[i] + fraction * m_current[i];
        if (!std::isfinite(value)) throw SimulationFault("nonfinite interpolated exhaust input");
        m_interpolated[i] = value;
    }
    std::int16_t sample;
    if (!m_source.native({m_interpolated.data(), m_interpolated.size()}, sample)) {
        throw SimulationFault("native synthesis made no progress");
    }
    return sample;
}

void SampleClock::interval(float *output) {
    const auto sample = nativeFrame(position());
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

void SampleClock::convertedNative() {
    const auto sample = nativeFrame(static_cast<double>(m_nativeInterpolationPhase) / NativeRate);
    if (!m_haveNative) { m_nativePrevious = m_nativeCurrent = sample; m_haveNative = true; }
    else { m_nativePrevious = m_nativeCurrent; m_nativeCurrent = sample; }
    m_nativeInterpolationPhase = (m_nativeInterpolationPhase + m_frequency) % NativeRate;
}

void SampleClock::convertedInterval(float *output) {
    std::uint32_t native = 0, ticks = 0;
    if (m_outputNativePhase == 0) { convertedNative(); ++native; }
    const auto fraction = static_cast<double>(m_outputNativePhase) / m_outputRate;
    const float sample = static_cast<float>(((1 - fraction) * m_nativePrevious + fraction * m_nativeCurrent) / 32768.0);
    if (!m_tickActive) startTick();
    auto nextTick = m_outputRate - m_outputTickPhase;
    auto nextNative = m_outputRate - m_outputNativePhase;
    // Event times inside this output interval are nextTick/F and nextNative/S.
    // Cross products are bounded by the configured rates, never lifetime counts.
    while (nextTick <= m_frequency || nextNative < NativeRate) {
        const bool dueNative = nextNative < NativeRate;
        const auto tickTime = std::uint64_t{nextTick} * NativeRate;
        const auto nativeTime = std::uint64_t{nextNative} * m_frequency;
        if (nextTick <= m_frequency && (!dueNative || tickTime <= nativeTime)) {
            finishTick(); ++ticks;
            if (nextTick < m_frequency && (!dueNative || tickTime != nativeTime)) startTick();
            nextTick += m_outputRate;
        }
        else {
            convertedNative(); ++native;
            // At a tie the completed tick precedes native synthesis and the
            // next latch follows it. A right-edge native event stays deferred.
            if (!m_tickActive) startTick();
            nextNative += m_outputRate;
        }
    }
    m_outputTickPhase = (m_outputTickPhase + m_frequency) % m_outputRate;
    m_outputNativePhase = (m_outputNativePhase + NativeRate) % m_outputRate;
    ++m_counts.frames; m_counts.ticks += ticks; m_counts.nativeFrames += native;
    *output = sample;
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
    if (!m_source.tick(m_next.data(), m_next.size())) throw SimulationFault("physics made no progress");
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
