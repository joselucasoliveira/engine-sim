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

PullEngine::PullEngine(Simulator &simulator, ControlLatch controls)
    : m_simulator(simulator), m_controls(std::move(controls)),
      m_clock(simulator.getSimulationFrequency(), preparedRest(simulator), *this) {
    m_simulator.synthesizer().beginSynchronous();
}

void PullEngine::latch() {
    if (m_controls) m_controls();
}

bool PullEngine::tick(double *endpoint) {
    m_simulator.advanceTick(false);
    std::copy_n(m_simulator.exhaustEndpoint(), m_simulator.getEngine()->getExhaustSystemCount(), endpoint);
    if (m_observer) m_observer(m_simulator);
    return true;
}

bool PullEngine::native(const double *input, std::int16_t &sample) {
    sample = m_simulator.synthesizer().renderNativeFrame(input);
    return true;
}
}
