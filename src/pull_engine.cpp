#include "../include/pull_engine.h"

#include <algorithm>
#include <stdexcept>

namespace engine_sim {
namespace {
std::vector<double> preparedRest(Simulator &simulator) {
    const auto *engine = simulator.getEngine();
    if (engine == nullptr || engine->getExhaustSystemCount() <= 0) {
        throw std::invalid_argument("pull engine requires a prepared simulator");
    }
    // Prepared speed, exhaust attenuation and delay histories are all zero.
    return std::vector<double>(engine->getExhaustSystemCount(), 0.0);
}
}

PullEngine::PullEngine(Simulator &simulator, Synthesizer &synthesis, ControlLatch controls,
        SampleClock::Profile profile, std::uint32_t outputRate)
    : m_simulator(simulator), m_synthesis(synthesis), m_controls(std::move(controls)),
      m_clock(simulator.getSimulationFrequency(), preparedRest(simulator), *this, profile, outputRate),
      m_flowTotals(simulator.getEngine()->getIntakeCount()) {
    m_synthesis.beginSynchronous();
}

void PullEngine::latch() {
    if (m_controls) m_controls();
    if (m_latchObserver) m_latchObserver(m_latchContext, m_clock.counts().frames, m_latchTick);
}

bool PullEngine::tick(double *endpoint, std::size_t capacity) {
    const auto status = tickChecked(endpoint, capacity);
    if (status == ProcessingStatus::physics_no_progress) return false;
    requireProcessing(status);
    return true;
}

ProcessingStatus PullEngine::tickChecked(double *endpoint, std::size_t capacity) {
    const auto count = m_simulator.acousticChannelCount();
    if (endpoint == nullptr || capacity < count) return ProcessingStatus::physics_no_progress;
    m_simulator.setSimulationFrequency(static_cast<int>(m_clock.tickFrequency()));
    if (m_beforeTick) m_beforeTick();
    AcousticExcitation excitation;
    const auto status = m_simulator.advanceTickChecked(capacity, excitation);
    if (status != ProcessingStatus::ok) return status;
    if (excitation.data == nullptr || excitation.count != count) return ProcessingStatus::physics_no_progress;
    const auto dt = m_simulator.getTimestep();
    for (std::size_t i = 0; i < m_flowTotals.size(); ++i) {
        m_simulator.getEngine()->getIntake(static_cast<int>(i))->m_flowRate /= dt;
    }
    if (m_presenting) {
        ++m_simulator.m_steps; ++m_simulator.m_currentIteration;
        m_presentationSeconds += dt;
        for (std::size_t i = 0; i < m_flowTotals.size(); ++i) {
            m_flowTotals[i] += m_simulator.getEngine()->getIntake(static_cast<int>(i))->m_flowRate * dt;
        }
    }
    std::copy_n(excitation.data, excitation.count, endpoint);
    if (m_observer) m_observer(m_simulator);
    if (m_afterTick) m_afterTick();
    const auto completed = m_checkedAfterTick ? m_checkedAfterTick() : ProcessingStatus::ok;
    if (m_latchObserver && completed == ProcessingStatus::ok) ++m_latchTick;
    return completed;
}

bool PullEngine::native(AcousticExcitation input, std::int16_t &sample) {
    requireProcessing(nativeChecked(input, sample));
    return true;
}

ProcessingStatus PullEngine::nativeChecked(AcousticExcitation input, std::int16_t &sample) {
    return m_synthesis.renderNativeFrameChecked(input, sample);
}

void PullEngine::beginPresentation() {
    m_simulator.m_steps = m_simulator.m_currentIteration = 0;
    std::fill(m_flowTotals.begin(), m_flowTotals.end(), 0.0);
    m_presentationSeconds = 0;
    m_presenting = true;
}

void PullEngine::endPresentation() {
    if (m_presentationSeconds > 0) {
        for (std::size_t i = 0; i < m_flowTotals.size(); ++i) {
            m_simulator.getEngine()->getIntake(static_cast<int>(i))->m_flowRate = m_flowTotals[i] / m_presentationSeconds;
        }
    }
    m_presenting = false;
}
}
