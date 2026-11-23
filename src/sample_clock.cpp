#include "../include/sample_clock.h"
#include "../include/simulation_fault.h"

#include <cmath>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace engine_sim {
namespace {
constexpr std::uint32_t maximumDesktopFrequency = 400000;

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
    const auto maximum = profile == Profile::Desktop ? maximumDesktopFrequency : 96000u;
    if (frequency < minimum || frequency > maximum || rest.empty()
        || (outputRate != NativeRate && outputRate != 48000)
        || (profile == Profile::Desktop && outputRate != NativeRate)) {
        throw std::invalid_argument("sample clock requires a supported frequency and exhaust channels");
    }
    for (double value : rest) {
        if (!std::isfinite(value)) throw std::invalid_argument("nonfinite prepared exhaust input");
    }
}

ProcessingStatus SampleClock::validateSpan(float *output, std::size_t count) const noexcept {
    if (m_faulted) return ProcessingStatus::clock_faulted;
    if (count > MaximumBlock || (count != 0 && output == nullptr)) {
        return ProcessingStatus::clock_span;
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    // At the maximum desktop rate even a full block cannot exceed this bound.
    // The exact phase calculation is needed only close to counter exhaustion.
    constexpr auto maximumTicks = (std::uint64_t{maximumDesktopFrequency} * MaximumBlock + NativeRate - 1) / NativeRate + 1;
    if (m_counts.frames <= maximum - MaximumBlock && m_counts.nativeFrames <= maximum - MaximumBlock
        && m_counts.ticks <= maximum - maximumTicks) {
        return ProcessingStatus::ok;
    }
    // Only the bounded request is multiplied. No lifetime frame product exists.
    const auto extraTicks = (m_variable || m_pendingFrequency) && count
        ? (std::uint64_t{maximumDesktopFrequency} * count + NativeRate - 1) / NativeRate + 1
        : (std::uint64_t{m_frequency} * count + m_phase) / NativeRate;
    const auto ticks = m_outputRate == NativeRate ? extraTicks
        : (std::uint64_t{m_frequency} * count + m_outputTickPhase) / m_outputRate;
    const auto native = m_outputRate == NativeRate ? count
        : (std::uint64_t{NativeRate} * count + m_outputNativePhase + m_outputRate - 1) / m_outputRate - (m_outputNativePhase != 0);
    if (count > maximum - m_counts.frames || native > maximum - m_counts.nativeFrames
        || ticks > maximum - m_counts.ticks) {
        return ProcessingStatus::clock_capacity;
    }
    return ProcessingStatus::ok;
}

void SampleClock::render(float *output, std::size_t count) {
    requireProcessing(renderChecked(output, count));
}

ProcessingStatus SampleClock::renderChecked(float *output, std::size_t count) {
    m_written = 0;
    if (const auto status = validateSpan(output, count); status != ProcessingStatus::ok) return status;
    try {
        while (m_written < count) {
            const auto status = m_outputRate == NativeRate ? interval(output + m_written) : convertedInterval(output + m_written);
            if (status != ProcessingStatus::ok) {
                m_faulted = true;
                return status;
            }
            ++m_written;
        }
    }
    catch (...) {
        m_faulted = true;
        throw;
    }
    return ProcessingStatus::ok;
}

ProcessingStatus SampleClock::nativeFrame(double fraction, std::int16_t &sample) {
    for (std::size_t i = 0; i < m_current.size(); ++i) {
        const double value = (1 - fraction) * m_previous[i] + fraction * m_current[i];
        if (!std::isfinite(value)) return ProcessingStatus::interpolated_exhaust;
        m_interpolated[i] = value;
    }
    return m_source.nativeChecked({m_interpolated.data(), m_interpolated.size()}, sample);
}

ProcessingStatus SampleClock::interval(float *output) {
    std::int16_t sample;
    if (const auto status = nativeFrame(position(), sample); status != ProcessingStatus::ok) return status;
    if (!m_tickActive) {
        if (m_pendingFrequency) {
            m_frequency = m_pendingFrequency;
            m_pendingFrequency = m_phase = m_variablePhase = 0;
            m_offset = 0; m_variable = false;
        }
        startTick();
    }
    std::uint32_t ticks = 0;
    ProcessingStatus status;
    if (m_pendingFrequency) {
        const long double duration = 1.0L / NativeRate;
        const long double remaining = duration - (1.0L - precisePosition()) / m_frequency;
        const long double tolerance = 64 * std::numeric_limits<long double>::epsilon() * duration;
        status = remaining > tolerance ? changeFrequencyWithinInterval(remaining, ticks)
            : m_variable ? advanceVariable(ticks) : advanceFixed(ticks);
    }
    else status = m_variable ? advanceVariable(ticks) : advanceFixed(ticks);
    if (status != ProcessingStatus::ok) return status;
    ++m_counts.frames;
    ++m_counts.nativeFrames;
    m_counts.ticks += ticks;
    *output = static_cast<float>(sample) / 32768.0f;
    return ProcessingStatus::ok;
}

ProcessingStatus SampleClock::convertedNative() {
    std::int16_t sample;
    if (const auto status = nativeFrame(static_cast<double>(m_nativeInterpolationPhase) / NativeRate, sample);
        status != ProcessingStatus::ok) return status;
    if (!m_haveNative) { m_nativePrevious = m_nativeCurrent = sample; m_haveNative = true; }
    else { m_nativePrevious = m_nativeCurrent; m_nativeCurrent = sample; }
    m_nativeInterpolationPhase = (m_nativeInterpolationPhase + m_frequency) % NativeRate;
    return ProcessingStatus::ok;
}

ProcessingStatus SampleClock::convertedInterval(float *output) {
    std::uint32_t native = 0, ticks = 0;
    if (m_outputNativePhase == 0) {
        if (const auto status = convertedNative(); status != ProcessingStatus::ok) return status;
        ++native;
    }
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
            if (const auto status = finishTick(); status != ProcessingStatus::ok) return status;
            ++ticks;
            if (nextTick < m_frequency && (!dueNative || tickTime != nativeTime)) startTick();
            nextTick += m_outputRate;
        }
        else {
            if (const auto status = convertedNative(); status != ProcessingStatus::ok) return status;
            ++native;
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
    return ProcessingStatus::ok;
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

ProcessingStatus SampleClock::finishTick() {
    if (const auto status = m_source.tickChecked(m_next.data(), m_next.size()); status != ProcessingStatus::ok) return status;
    for (double value : m_next) {
        if (!std::isfinite(value)) return ProcessingStatus::exhaust_endpoint;
    }
    m_previous.swap(m_current);
    m_current.swap(m_next);
    m_tickActive = false;
    return ProcessingStatus::ok;
}

ProcessingStatus SampleClock::advanceFixed(std::uint32_t &ticks) {
    const auto total = m_phase + m_frequency;
    ticks = total / NativeRate;
    const auto remainder = total % NativeRate;
    for (std::uint32_t i = 0; i < ticks; ++i) {
        if (const auto status = finishTick(); status != ProcessingStatus::ok) return status;
        if (i + 1 < ticks || remainder != 0) {
            startTick();
        }
    }
    m_phase = remainder;
    return ProcessingStatus::ok;
}

ProcessingStatus SampleClock::advanceVariable(std::uint32_t &ticks) {
    const auto total = m_variablePhase + m_frequency;
    const auto next = total % NativeRate;
    const auto oldCycles = roundedCycles(m_offset + static_cast<long double>(m_variablePhase) / NativeRate);
    const auto newCycles = roundedCycles(m_offset + static_cast<long double>(next) / NativeRate);
    ticks = static_cast<std::uint32_t>(total / NativeRate
        + static_cast<int>(std::floor(newCycles)) - static_cast<int>(std::floor(oldCycles)));
    const bool atRightEdge = newCycles == std::floor(newCycles);
    for (std::uint32_t i = 0; i < ticks; ++i) {
        if (const auto status = finishTick(); status != ProcessingStatus::ok) return status;
        if (i + 1 < ticks || !atRightEdge) startTick();
    }
    m_variablePhase = next;
    return ProcessingStatus::ok;
}

ProcessingStatus SampleClock::changeFrequencyWithinInterval(long double remaining, std::uint32_t &completed) {
    if (const auto status = finishTick(); status != ProcessingStatus::ok) return status;
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
        if (const auto status = finishTick(); status != ProcessingStatus::ok) return status;
        if (i + 1 < ticks || m_offset != 0) startTick();
    }
    completed = ticks + 1;
    return ProcessingStatus::ok;
}

void SampleClock::requestDesktopFrequency(std::uint32_t frequency) {
    if (m_profile != Profile::Desktop || frequency < 400 || frequency > maximumDesktopFrequency) {
        throw std::invalid_argument("invalid private desktop simulation frequency");
    }
    m_pendingFrequency = frequency == m_frequency ? 0 : frequency;
}
}
