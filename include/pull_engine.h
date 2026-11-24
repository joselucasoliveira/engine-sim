#ifndef ENGINE_SIM_PULL_ENGINE_H
#define ENGINE_SIM_PULL_ENGINE_H

#include "sample_clock.h"
#include "simulator.h"
#include "synthesizer.h"

#include <functional>

namespace engine_sim {
// Owned by the synchronous host, with the simulator alive for its full lifetime.
class PullEngine final : private ClockSource {
public:
    using ControlLatch = std::function<void()>;
    using CheckedCommand = std::function<ProcessingStatus()>;
    using TickObserver = std::function<void(const Simulator &)>;
    using LatchObserver = void (*)(void *, std::uint64_t localFrame, std::uint64_t tick) noexcept;
    PullEngine(Simulator &simulator, Synthesizer &synthesis, ControlLatch controls,
        SampleClock::Profile profile = SampleClock::Profile::Public,
        std::uint32_t outputRate = SampleClock::NativeRate);
    PullEngine(const PullEngine &) = delete;
    PullEngine &operator=(const PullEngine &) = delete;
    void render(float *output, std::size_t count) { m_clock.render(output, count); }
    ProcessingStatus renderChecked(float *output, std::size_t count) { return m_clock.renderChecked(output, count); }
    const ClockCounts &counts() const { return m_clock.counts(); }
    std::size_t lastWritten() const { return m_clock.lastWritten(); }
    bool faulted() const { return m_clock.faulted(); }
    void setTickObserver(TickObserver observer) { m_observer = std::move(observer); }
    void setLatchObserver(LatchObserver observer, void *context) noexcept {
        m_latchObserver = observer;
        m_latchContext = context;
        m_latchTick = m_clock.counts().ticks;
    }
    void setTickCommands(ControlLatch before, ControlLatch after) {
        m_beforeTick = std::move(before);
        m_afterTick = std::move(after);
        m_checkedAfterTick = {};
    }
    void setCheckedTickCommands(ControlLatch before, CheckedCommand after) {
        m_beforeTick = std::move(before);
        m_afterTick = {};
        m_checkedAfterTick = std::move(after);
    }
    void requestDesktopFrequency(std::uint32_t frequency) { m_clock.requestDesktopFrequency(frequency); }
    void beginPresentation();
    void endPresentation();

private:
    friend struct ClockTestAccess;
    void latch() override;
    bool tick(double *endpoint, std::size_t capacity) override;
    bool native(AcousticExcitation input, std::int16_t &sample) override;
    ProcessingStatus tickChecked(double *endpoint, std::size_t capacity) override;
    ProcessingStatus nativeChecked(AcousticExcitation input, std::int16_t &sample) override;
    Simulator &m_simulator;
    Synthesizer &m_synthesis;
    ControlLatch m_controls;
    TickObserver m_observer;
    ControlLatch m_beforeTick, m_afterTick;
    CheckedCommand m_checkedAfterTick;
    SampleClock m_clock;
    LatchObserver m_latchObserver = nullptr;
    void *m_latchContext = nullptr;
    std::uint64_t m_latchTick = 0;
    std::vector<double> m_flowTotals;
    double m_presentationSeconds = 0;
    bool m_presenting = false;
};
}
#endif
