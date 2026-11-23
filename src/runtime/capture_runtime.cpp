#include "capture_runtime.hpp"
#include "simulation_fault.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace engine_sim_capture {
void Range::add(double value) {
    valid = valid && std::isfinite(value);
    min = std::min(min, value); max = std::max(max, value);
}
RuntimeHost::RuntimeHost()
    : m_runtime(es_config{sizeof(es_config), ES_ABI_V2, 44100, ES_FREE, 1, 0, 0, 0},
          engine_sim::SampleClock::Profile::Desktop) {}
std::unique_ptr<RuntimeHost> RuntimeHost::prepare(const es_script::PreparationContext &recipe,
        std::uint64_t seed, bool mechanics) {
    engine_sim::PreparationConfig config;
    config.desktop = true; config.seed = seed;
    auto prepared = engine_sim::prepareEngine(recipe, config);
    const auto frequency = prepared.session->simulator->getSimulationFrequency();
    if (frequency < 4000 || frequency > 96000 || prepared.session->definition.engine->getCylinderCount() > 64)
        throw std::invalid_argument("Capture diagnostics support 4000..96000 Hz and at most 64 cylinders");
    auto result = std::unique_ptr<RuntimeHost>(new RuntimeHost);
    result->m_mechanics = mechanics;
    auto *owner = result.get();
    engine_sim::legacy_runtime::Access::adopt(result->m_runtime, std::move(prepared), [owner] { owner->latch(); });
    result->m_runtime.m_session->pull->setTickObserver([owner](const Simulator &sim) { owner->observe(sim); });
    return result;
}
void RuntimeHost::setControls(Controls controls) {
    if (!std::isfinite(controls.demand) || controls.demand < 0 || controls.demand > 1 ||
        !std::isfinite(controls.dynoSpeed) || controls.dynoSpeed < 0)
        throw std::invalid_argument("Invalid capture controls");
    m_controls = controls;
}
void RuntimeHost::latch() {
    m_applied = m_controls;
    auto &sim = *m_runtime.m_session->prepared.session->simulator;
    sim.getEngine()->setSpeedControl(m_controls.demand);
    sim.getEngine()->getIgnitionModule()->m_enabled = m_controls.ignition;
    sim.m_starterMotor.m_enabled = m_controls.starter;
    sim.m_dyno.m_enabled = sim.m_dyno.m_hold = m_controls.dyno;
    if (m_controls.dyno) sim.m_dyno.m_rotationSpeed = m_controls.dynoSpeed;
}
TickSample RuntimeHost::current() const {
    const auto &sim = simulator();
    const auto &model = engine();
    const auto &crank = *model.getOutputCrankshaft();
    return {m_observedTicks, 0, crank.getCycleAngle(), crank.m_body.v_theta,
        m_applied.demand, model.getThrottlePlateAngle(), sim.m_starterMotor.m_enabled,
        model.getIgnitionModule()->m_enabled};
}
void RuntimeHost::observe(const Simulator &sim) {
    if (!m_block || m_diagnostics.ticks == m_diagnostics.samples.size())
        throw engine_sim::SimulationFault("Capture tick diagnostic capacity exceeded");
    ++m_observedTicks;
    const auto &model = *sim.getEngine();
    m_diagnostics.manifold.add(model.getManifoldPressure());
    for (int cylinder = 0; cylinder < model.getCylinderCount(); ++cylinder) {
        const auto &gas = model.getChamber(cylinder)->m_system;
        m_diagnostics.pressure.add(gas.pressure());
        m_diagnostics.moles.add(gas.n()); m_diagnostics.volume.add(gas.volume());
    }
    if (m_mechanics) {
        auto sample = current();
        for (int cylinder = 0; cylinder < model.getCylinderCount(); ++cylinder) {
            // Consumption is private to this diagnostic owner, after the tick.
            if (m_runtime.m_session->prepared.session->definition.engine->getChamber(cylinder)->popLitLastFrame())
                sample.lit |= std::uint64_t{1} << cylinder;
        }
        m_diagnostics.samples[m_diagnostics.ticks] = sample;
    }
    ++m_diagnostics.ticks;
}
void RuntimeHost::beginBlock() {
    if (m_block || telemetry().state != ES_READY) throw std::logic_error("Capture block requires ready quiescent runtime");
    m_diagnostics = {}; m_blockFrames = 0; m_block = true;
    m_runtime.m_session->pull->beginPresentation();
}
void RuntimeHost::render(float *output, std::uint32_t frames, std::uint32_t &written) {
    if (!m_block || frames > 441 - m_blockFrames || written != 0 || (frames && !output))
        throw std::invalid_argument("Capture block exceeds bounded 10 ms output storage");
    if (telemetry().state != ES_READY) throw engine_sim::SimulationFault("Capture runtime is faulted");
    try { m_runtime.render(output, frames, written); }
    catch (...) { m_blockFrames += written; throw; }
    m_blockFrames += written;
}
void RuntimeHost::endBlock() {
    if (!m_block) throw std::logic_error("No capture block is active");
    m_runtime.m_session->pull->endPresentation(); m_block = false;
}
}
