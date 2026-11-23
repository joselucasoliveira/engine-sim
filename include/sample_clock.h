#ifndef ENGINE_SIM_SAMPLE_CLOCK_H
#define ENGINE_SIM_SAMPLE_CLOCK_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine_sim {
struct ClockTestAccess;

struct ClockCounts {
    std::uint64_t frames = 0;
    std::uint64_t ticks = 0;
    std::uint64_t nativeFrames = 0;
};

class ClockSource {
public:
    virtual ~ClockSource() = default;
    virtual void latch() = 0;
    // Reject a null or undersized destination before stepping. Any caller
    // storage beyond the configured channel count is left untouched.
    virtual bool tick(double *endpoint, std::size_t capacity) = 0;
    virtual bool native(const double *input, std::int16_t &sample) = 0;
};

// Private C++ backend. A source tick and a native sample each run exactly once.
class SampleClock {
public:
    enum class Profile { Public, Desktop };
    static constexpr std::uint32_t NativeRate = 44100;
    static constexpr std::size_t MaximumBlock = 65536;
    SampleClock(std::uint32_t frequency, const std::vector<double> &rest, ClockSource &source,
        Profile profile = Profile::Public, std::uint32_t outputRate = NativeRate);
    SampleClock(const SampleClock &) = delete;
    SampleClock &operator=(const SampleClock &) = delete;
    void render(float *output, std::size_t count);
    const ClockCounts &counts() const { return m_counts; }
    std::size_t lastWritten() const { return m_written; }
    bool faulted() const { return m_faulted; }
    void requestDesktopFrequency(std::uint32_t frequency);
    std::uint32_t tickFrequency() const { return m_frequency; }

private:
    friend struct ClockTestAccess;
    void validateSpan(float *output, std::size_t count) const;
    void interval(float *output);
    void convertedInterval(float *output);
    std::int16_t nativeFrame(double fraction);
    void convertedNative();
    void finishTick();
    void startTick();
    double position() const;
    long double precisePosition() const;
    std::uint32_t advanceFixed();
    std::uint32_t advanceVariable();
    std::uint32_t changeFrequencyWithinInterval(long double remaining);
    std::uint32_t m_frequency;
    std::uint32_t m_outputRate;
    std::uint32_t m_phase = 0;
    ClockSource &m_source;
    ClockCounts m_counts;
    std::vector<double> m_previous, m_current, m_next, m_interpolated;
    std::size_t m_written = 0;
    bool m_tickActive = false;
    bool m_faulted = false;
    Profile m_profile;
    std::uint32_t m_pendingFrequency = 0, m_variablePhase = 0;
    long double m_offset = 0;
    bool m_variable = false;
    std::uint32_t m_outputTickPhase = 0, m_outputNativePhase = 0, m_nativeInterpolationPhase = 0;
    std::int16_t m_nativePrevious = 0, m_nativeCurrent = 0;
    bool m_haveNative = false;
};
}
#endif
