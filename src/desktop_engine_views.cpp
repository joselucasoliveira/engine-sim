#include "../include/desktop_engine_views.h"
#include "../include/constants.h"
#include <atomic>
#include <cmath>

namespace desktop {
EngineViews::EngineViews(const Simulator &simulator, const Synthesizer &synthesis)
    : m_epoch(std::make_shared<ViewEpoch>()) {
    static std::atomic<std::uint64_t> next{1};
    m_epoch->generation = next.fetch_add(1, std::memory_order_relaxed);
    add(&simulator);
    add(&synthesis);
    const auto *engine = simulator.getEngine();
    add(engine);
    for (int i = 0; i < engine->getCylinderCount(); ++i) {
        add(engine->getPiston(i)); add(engine->getConnectingRod(i)); add(engine->getChamber(i));
    }
    for (int i = 0; i < engine->getCylinderBankCount(); ++i) {
        add(engine->getCylinderBank(i)); add(engine->getHead(i));
    }
    for (int i = 0; i < engine->getCrankshaftCount(); ++i) add(engine->getCrankshaft(i));
    m_fired.resize(engine->getCylinderCount());
}
void EngineViews::requireQuiescent() const {
    if (!m_epoch->alive || m_epoch->owner != std::this_thread::get_id() || m_epoch->readers)
        throw std::logic_error("Engine mutation requires a quiescent owner boundary");
}
void EngineViews::collectIgnition(Simulator &simulator) {
    requireQuiescent();
    for (int i = 0; i < simulator.getEngine()->getCylinderCount(); ++i)
        m_fired[i] = simulator.getEngine()->getChamber(i)->popLitLastFrame();
}
void EngineViews::sampleTick(const Simulator &simulator) {
    requireQuiescent();
    ++m_ticks;
    if (simulator.getCurrentIteration() % 2) return;
    const auto *engine = simulator.getEngine();
    const auto *chamber = engine->getChamber(0);
    double cycle = engine->getCrankshaft(0)->getCycleAngle();
    if (!engine->isSpinningCw()) cycle = 4 * constants::pi - cycle;
    const auto *head = chamber->getCylinderHead();
    const int cylinder = chamber->getPiston()->getCylinderIndex();
    ScopeObservation point{m_ticks, cycle, engine->getCrankshaft(0)->getCycleAngle(constants::pi),
        simulator.getTotalExhaustFlow() / simulator.getTimestep(),
        std::sqrt(chamber->m_system.pressure() + chamber->m_system.dynamicPressure(-1, 0)),
        chamber->getLastTimestepExhaustFlow() / simulator.getTimestep(),
        chamber->getLastTimestepIntakeFlow() / simulator.getTimestep(), chamber->m_system.n(),
        head->exhaustValveLift(cylinder), head->intakeValveLift(cylinder), chamber->getVolume(),
        std::sqrt(chamber->m_system.pressure()), units::toRpm(engine->getSpeed())};
    if (m_size == ScopeCapacity) { m_head = (m_head + 1) % ScopeCapacity; --m_size; ++m_evicted; }
    m_scopes[(m_head + m_size++) % ScopeCapacity] = point;
}
bool EngineViews::popScope(ScopeObservation &point) {
    requireQuiescent();
    if (!m_size) return false;
    point = m_scopes[m_head]; m_head = (m_head + 1) % ScopeCapacity; --m_size;
    return true;
}
EngineCommands::EngineCommands(Simulator &simulator, Synthesizer &synthesis, EngineViews &views)
    : m_simulator(simulator), m_synthesis(synthesis), m_views(views),
      m_gear(simulator.getTransmission()->getGear()), m_mixer(synthesis.getAudioParameters()) {
    pending = {simulator.getEngine()->getSpeedControl(), simulator.getTransmission()->getClutchPressure(),
        simulator.m_dyno.m_rotationSpeed, simulator.getEngine()->getIgnitionModule()->m_enabled,
        simulator.m_starterMotor.m_enabled, simulator.m_dyno.m_enabled, simulator.m_dyno.m_hold,
        simulator.getSimulationFrequency()};
    applied = pending;
}
void EngineCommands::apply(const Values &value, bool frequency) {
    m_views.requireQuiescent();
    m_simulator.getEngine()->setSpeedControl(value.demand);
    m_simulator.getEngine()->getIgnitionModule()->m_enabled = value.ignition;
    m_simulator.m_starterMotor.m_enabled = value.starter;
    m_simulator.getTransmission()->setClutchPressure(value.clutch);
    m_simulator.m_dyno.m_rotationSpeed = value.dynoSpeed;
    m_simulator.m_dyno.m_enabled = value.dyno; m_simulator.m_dyno.m_hold = value.hold;
    if (frequency) m_simulator.setSimulationFrequency(value.frequency);
}
void EngineCommands::flush() {
    m_views.requireQuiescent();
    if (m_simulator.getTransmission()->getGear() != m_gear) m_simulator.getTransmission()->changeGear(m_gear);
    if (m_mixerDirty) { m_synthesis.setAudioParameters(m_mixer); m_mixerDirty = false; }
    apply(pending, true);
}
}
