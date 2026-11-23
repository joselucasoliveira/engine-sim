#include "runtime.hpp"
#include "simulation_fault.h"
#include "constants.h"
#include "piston_engine_simulator.h"
#include "shaft_drive.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace engine_sim {
Runtime::Runtime(es_config config, SampleClock::Profile profile) noexcept : m_config(config), m_profile(profile) { unload(); }
Runtime::Session::Session(std::unique_ptr<EngineSession> value, Recipe source, std::uint32_t mode, bool desktop,
        std::uint32_t outputHz, SampleClock::Profile profile, PullEngine::ControlLatch controls)
    : prepared{std::move(source), std::move(value)} {
    auto &sim = *prepared.session->simulator;
    if (profile == SampleClock::Profile::Public) {
        if (sim.getTransmission()->getGear() != -1) sim.getTransmission()->changeGear(-1);
        sim.getTransmission()->setClutchPressure(0);
        sim.m_dyno.m_enabled = sim.m_dyno.m_hold = false;
        const auto &system = *sim.getSystem();
        const auto objects = system.getConstraintCount();
        const auto rows = system.getFullConstraintCount();
        const auto generators = system.getForceGeneratorCount();
        if (mode == ES_DRIVEN) drive = std::make_unique<DrivenShaft>(sim);
        else static_cast<PistonEngineSimulator &>(sim).enableExternalLoad();
        if (system.getConstraintCount() - objects != 1 || system.getFullConstraintCount() - rows != 1 ||
            system.getForceGeneratorCount() - generators != (drive ? 1 : 0))
            throw SimulationFault("Unexpected final public mode solver topology");
        sim.prepareFixedWorkspace();
    }
    else if (!desktop || !controls) {
        throw std::logic_error("Desktop session requires detached owner commands");
    }
    if (!controls) controls = [this] { latch(); };
    pull = std::make_unique<PullEngine>(sim, prepared.session->synthesis, std::move(controls), profile, outputHz);
    if (drive) pull->setCheckedTickCommands([this] { drive->beginTick(); }, [this] { return drive->finishTick(); });
}
Runtime::Session::~Session() = default;
void Runtime::Session::latch() {
    auto &sim = *prepared.session->simulator;
    sim.getEngine()->setSpeedControl(requested.demand);
    sim.getEngine()->getIgnitionModule()->m_enabled = requested.ignition != 0;
    sim.m_starterMotor.m_enabled = !drive && requested.starter != 0;
    if (drive) drive->latch(requested.driven_rpm, requested.load_nm);
    else static_cast<PistonEngineSimulator &>(sim).setExternalLoadNm(requested.load_nm);
}
ProcessingStatus Runtime::snapshot(const Session &session, es_telemetry &value) const noexcept {
    const auto &sim = *session.prepared.session->simulator;
    auto *engine = sim.getEngine();
    auto *crank = engine->getCrankshaft(0);
    const auto &counts = session.pull->counts();
    value = {};
    value.size = sizeof(value); value.abi = ES_ABI_V2; value.state = ES_READY; value.mode = m_config.mode;
    value.output_hz = m_config.output_hz; value.simulation_hz = sim.getSimulationFrequency();
    value.cylinders = static_cast<std::uint32_t>(engine->getCylinderCount());
    value.frames = counts.frames; value.ticks = counts.ticks; value.native_frames = counts.nativeFrames;
    value.crank_rad_s = crank->m_body.v_theta;
    value.measured_rpm = std::abs(value.crank_rad_s) * 60 / (2 * constants::pi);
    value.cycle_rad = crank->getCycleAngle(); value.plate_angle_rad = engine->getThrottlePlateAngle();
    value.displacement_m3 = engine->getDisplacement();
    value.requested_load_nm = session.requested.load_nm;
    value.load_reaction_nm = static_cast<const PistonEngineSimulator &>(sim).getExternalLoadReactionNm();
    if (!std::isfinite(value.load_reaction_nm)) return ProcessingStatus::load_reaction;
    if (session.drive) {
        value.driven_limit_rpm = std::min(12000., units::toRpm(engine->getRedline()));
        value.load_reaction_nm = session.drive->loadReactionNm();
        value.drive_reaction_nm = session.drive->reactionNm();
        value.applied_driven_rpm = session.drive->appliedRpm();
    }
    for (double field : {value.crank_rad_s, value.measured_rpm, value.cycle_rad, value.plate_angle_rad,
            value.displacement_m3, value.load_reaction_nm, value.drive_reaction_nm, value.applied_driven_rpm}) {
        if (!std::isfinite(field)) return ProcessingStatus::telemetry_nonfinite;
    }
    return ProcessingStatus::ok;
}
void Runtime::adoptSession(std::unique_ptr<EngineSession> engine, Recipe recipe, std::uint32_t mode, bool desktop,
        PullEngine::ControlLatch controls, const std::function<void()> &beforeCommit) {
    auto candidate = std::make_unique<Session>(std::move(engine), std::move(recipe), mode, desktop,
                                               m_config.output_hz, m_profile, std::move(controls));
    es_telemetry initial{};
    requireProcessing(snapshot(*candidate, initial));
    if (beforeCommit) beforeCommit();
    m_session.swap(candidate);
    m_snapshot = initial;
    m_processingFailure = ProcessingStatus::ok;
}
void Runtime::setControls(const es_controls &controls) noexcept {
    m_session->requested = controls;
    m_snapshot.requested_load_nm = controls.load_nm;
}
void Runtime::render(float *output, std::uint32_t frames, std::uint32_t &written) {
    requireProcessing(renderChecked(output, frames, written));
}

ProcessingStatus Runtime::renderChecked(float *output, std::uint32_t frames, std::uint32_t &written) {
    if (!m_session || m_snapshot.state != ES_READY) return ProcessingStatus::clock_faulted;
    if (frames != 0 && !output) return ProcessingStatus::clock_span;
    if (m_profile == SampleClock::Profile::Public) {
        const auto &counts = m_session->pull->counts();
        const auto rate = m_config.output_hz;
        const auto frequency = m_snapshot.simulation_hz;
        const auto phase = ((counts.frames % rate) * frequency) % rate;
        const auto extraTicks = (std::uint64_t{frames} * frequency + phase) / rate;
        const auto nativePhase = ((counts.frames % rate) * SampleClock::NativeRate) % rate;
        const auto extraNative = (std::uint64_t{frames} * SampleClock::NativeRate + nativePhase + rate - 1) / rate - (nativePhase != 0);
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (frames > maximum - counts.frames || extraNative > maximum - counts.nativeFrames || extraTicks > maximum - counts.ticks)
            return ProcessingStatus::render_capacity;
    }
    // The desktop's live numerical-rate profile is checked by SampleClock using
    // its retained physical phase; a lifetime N*currentF formula would be wrong.
    try {
        while (written < frames) {
            float sample;
            es_telemetry next{};
            auto status = m_session->pull->renderChecked(&sample, 1);
            if (status == ProcessingStatus::ok) status = snapshot(*m_session, next);
            if (status != ProcessingStatus::ok) {
                m_snapshot.state = ES_FAULTED;
                m_processingFailure = status;
                return status;
            }
            output[written++] = sample;
            m_snapshot = next;
        }
    }
    catch (...) { m_snapshot.state = ES_FAULTED; throw; }
    return ProcessingStatus::ok;
}
void Runtime::unload() noexcept {
    m_session.reset(); m_snapshot = {};
    m_snapshot.size = sizeof(m_snapshot); m_snapshot.abi = ES_ABI_V2;
    m_snapshot.state = ES_EMPTY; m_snapshot.mode = m_config.mode; m_snapshot.output_hz = m_config.output_hz;
    m_processingFailure = ProcessingStatus::ok;
}
}
