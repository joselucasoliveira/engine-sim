#ifndef ENGINE_SIM_TEST_LEGACY_ACCESS_H
#define ENGINE_SIM_TEST_LEGACY_ACCESS_H

#include "simulator.h"
#include "synthesizer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace engine_sim_test {
// The historical block scheduler is confined to primitive regression tests.
class LegacyFrame {
public:
    LegacyFrame(Simulator &simulator, Synthesizer &synthesis)
        : m_simulator(simulator), m_synthesis(synthesis),
          m_flows(simulator.getEngine() == nullptr ? 0 : simulator.getEngine()->getIntakeCount()) {}

    void setTargetLatency(double value) { m_targetLatency = value; }

    void startFrame(double dt) {
        if (!begin()) return;
        auto steps = static_cast<int>(std::round(dt * m_simulator.getSimulationSpeed() / m_simulator.getTimestep()));
        if (m_synthesis.getLatency() < m_targetLatency) steps = static_cast<int>((steps + 1) * 1.1);
        else if (m_synthesis.getLatency() > m_targetLatency) steps = std::max(0, static_cast<int>((steps - 1) * 0.9));
        resetFlows(steps);
    }

    void startFixedFrame(int ticks) {
        if (ticks <= 0) throw std::invalid_argument("Legacy test needs positive ticks");
        if (begin()) resetFlows(ticks);
    }

    bool simulateStep() {
        if (m_simulator.m_currentIteration >= m_simulator.m_steps) return false;
        const auto endpoint = m_simulator.advanceTick(m_simulator.acousticChannelCount());
        if (endpoint.data == nullptr) throw std::logic_error("Legacy test requires a prepared simulator tick");
        for (std::size_t i = 0; i < m_flows.size(); ++i) {
            auto &flow = m_simulator.getEngine()->getIntake(static_cast<int>(i))->m_flowRate;
            m_flows[i] += flow;
            flow = m_flows[i];
        }
        m_synthesis.writeInput(endpoint.data);
        ++m_simulator.m_currentIteration;
        return true;
    }

    void endFrame() {
        m_synthesis.endInputBlock();
        const auto *engine = m_simulator.getEngine();
        if (engine == nullptr) return;
        const double span = m_simulator.m_steps * m_simulator.getTimestep();
        for (int i = 0; i < engine->getIntakeCount(); ++i) {
            auto &flow = m_simulator.getEngine()->getIntake(i)->m_flowRate;
            flow = span > 0 ? flow / span : 0;
        }
    }

private:
    bool begin() {
        if (m_simulator.getEngine() == nullptr) {
            m_simulator.m_steps = 0;
            return false;
        }
        m_simulator.m_currentIteration = 0;
        m_synthesis.setInputSampleRate(m_simulator.getSimulationFrequency() * m_simulator.getSimulationSpeed());
        return true;
    }

    void resetFlows(int steps) {
        m_simulator.m_steps = steps;
        if (steps <= 0) return;
        for (std::size_t i = 0; i < m_flows.size(); ++i) {
            m_simulator.getEngine()->getIntake(static_cast<int>(i))->m_flowRate = 0;
            m_flows[i] = 0;
        }
    }

    Simulator &m_simulator;
    Synthesizer &m_synthesis;
    std::vector<double> m_flows;
    double m_targetLatency = 0.1;
};
}
#endif
