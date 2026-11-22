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
    virtual bool tick(double *endpoint) = 0;
    virtual bool native(const double *input, std::int16_t &sample) = 0;
};

// Private C++ backend. A source tick and a native sample each run exactly once.
class SampleClock {
public:
    static constexpr std::uint32_t NativeRate = 44100;
    static constexpr std::size_t MaximumBlock = 65536;
    SampleClock(std::uint32_t frequency, const std::vector<double> &rest, ClockSource &source);
    SampleClock(const SampleClock &) = delete;
    SampleClock &operator=(const SampleClock &) = delete;
    void render(float *output, std::size_t count);
    const ClockCounts &counts() const { return m_counts; }
    std::size_t lastWritten() const { return m_written; }
    bool faulted() const { return m_faulted; }

private:
    friend struct ClockTestAccess;
    void validateSpan(float *output, std::size_t count) const;
    void interval(float *output);
    std::uint32_t m_frequency;
    std::uint32_t m_phase = 0;
    ClockSource &m_source;
    ClockCounts m_counts;
    std::vector<double> m_previous, m_current, m_next, m_interpolated;
    std::size_t m_written = 0;
    bool m_tickActive = false;
    bool m_faulted = false;
};
}
#endif
