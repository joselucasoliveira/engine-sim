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
    PullEngine(Simulator &simulator, ControlLatch controls);
    PullEngine(const PullEngine &) = delete;
    PullEngine &operator=(const PullEngine &) = delete;
    void render(float *output, std::size_t count) { m_clock.render(output, count); }
    const ClockCounts &counts() const { return m_clock.counts(); }
    std::size_t lastWritten() const { return m_clock.lastWritten(); }
    bool faulted() const { return m_clock.faulted(); }
    void setTickObserver(TickObserver observer) { m_observer = std::move(observer); }

private:
    void latch() override;
    bool tick(double *endpoint) override;
    bool native(const double *input, std::int16_t &sample) override;
    Simulator &m_simulator;
    ControlLatch m_controls;
    TickObserver m_observer;
    SampleClock m_clock;
};
}
#endif
