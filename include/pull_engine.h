#ifndef ENGINE_SIM_PULL_ENGINE_H
#define ENGINE_SIM_PULL_ENGINE_H

#include "sample_clock.h"
#include "simulator.h"

#include <functional>

namespace engine_sim {
// Owned by the synchronous host, with the simulator alive for its full lifetime.
class PullEngine final : private ClockSource {
public:
    using ControlLatch = std::function<void()>;
    using TickObserver = std::function<void(const Simulator &)>;
    PullEngine(Simulator &simulator, ControlLatch controls, SampleClock::Profile profile = SampleClock::Profile::Public,
        std::uint32_t outputRate = SampleClock::NativeRate);
    PullEngine(const PullEngine &) = delete;
    PullEngine &operator=(const PullEngine &) = delete;
    void render(float *output, std::size_t count) { m_clock.render(output, count); }
    const ClockCounts &counts() const { return m_clock.counts(); }
    std::size_t lastWritten() const { return m_clock.lastWritten(); }
    bool faulted() const { return m_clock.faulted(); }
    void setTickObserver(TickObserver observer) { m_observer = std::move(observer); }
    void setTickCommands(ControlLatch before, ControlLatch after) { m_beforeTick = std::move(before); m_afterTick = std::move(after); }
    void requestDesktopFrequency(std::uint32_t frequency) { m_clock.requestDesktopFrequency(frequency); }
    void beginPresentation();
    void endPresentation();

private:
    friend struct ClockTestAccess;
    void latch() override;
    bool tick(double *endpoint, std::size_t capacity) override;
    bool native(const double *input, std::int16_t &sample) override;
    Simulator &m_simulator;
    ControlLatch m_controls;
    TickObserver m_observer;
    ControlLatch m_beforeTick, m_afterTick;
    SampleClock m_clock;
    std::vector<double> m_flowTotals;
    double m_presentationSeconds = 0;
    bool m_presenting = false;
};
}
#endif
