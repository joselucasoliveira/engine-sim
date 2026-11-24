#include "../include/simulator.h"
#include "../include/checked_solver.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace {
class OwnedOptimizedSystem : public atg_scs::OptimizedNsvRigidBodySystem {
public:
    OwnedOptimizedSystem() { initialize(&m_solver); }

    void prepareFixedWorkspace() {
        if (m_prepared || m_rigidBodies.empty() || m_rigidBodies.size() > 255 || m_constraints.empty() ||
            m_constraints.size() > std::numeric_limits<int>::max() ||
            m_forceGenerators.size() > std::numeric_limits<int>::max())
            throw engine_sim::SimulationFault("Invalid fixed solver graph");

        const auto bodies = static_cast<int>(m_rigidBodies.size());
        for (int i = 0; i < bodies; ++i) {
            const auto *body = m_rigidBodies[i];
            if (!body || body->index != i)
                throw engine_sim::SimulationFault("Invalid fixed solver body index");
        }
        int rows = 0;
        constexpr auto maximum = std::numeric_limits<int>::max();
        for (const auto *constraint : m_constraints) {
            if (!constraint || constraint->m_bodyCount < 1 || constraint->m_bodyCount > atg_scs::Constraint::MaxBodyCount)
                throw engine_sim::SimulationFault("Unsupported fixed solver constraint");
            const auto count = constraint->getConstraintCount();
            if (count < 1 || count > atg_scs::Constraint::MaxConstraintCount || count > maximum - rows)
                throw engine_sim::SimulationFault("Invalid fixed solver row count");
            rows += count;
            for (int lane = 0; lane < constraint->m_bodyCount; ++lane) {
                const auto *body = constraint->m_bodies[lane];
                if (!body || body->index < 0 || body->index >= bodies || m_rigidBodies[body->index] != body)
                    throw engine_sim::SimulationFault("Invalid fixed solver row body binding");
            }
        }
        if (rows == 0)
            throw engine_sim::SimulationFault("Invalid fixed solver row count");
        if (rows > maximum / rows || static_cast<std::size_t>(rows) >
                std::numeric_limits<std::size_t>::max() / sizeof(double) / static_cast<std::size_t>(rows))
            throw engine_sim::SimulationFault("Fixed solver matrix extent exceeds addressability");

        m_state.resize(bodies, rows);
        for (auto *array : {m_state.a_theta, m_state.v_theta, m_state.theta, m_state.a_x, m_state.a_y,
                m_state.v_x, m_state.v_y, m_state.p_x, m_state.p_y, m_state.f_x, m_state.f_y,
                m_state.t, m_state.m})
            std::fill_n(array, bodies, 0.0);
        std::fill_n(m_state.indexMap, rows, 0);
        for (auto *array : {m_state.r_x, m_state.r_y, m_state.r_t})
            std::fill_n(array, 2 * rows, 0.0);

        const auto coordinates = 3 * bodies;
        for (auto *matrix : {&m_iv.M, &m_iv.M_inv, &m_iv.q_dot, &m_iv.q_dot_prime, &m_iv.F_ext})
            matrix->initialize(1, coordinates);
        for (auto *matrix : {&m_iv.v_bias, &m_iv.C, &m_iv.b_err, &m_iv.right, &m_iv.lambda})
            matrix->initialize(1, rows);
        m_iv.limits.initialize(2, rows);
        m_iv.reg0.initialize(1, std::max(coordinates, rows));
        m_iv.reg1.initialize(1, std::max(coordinates, rows));
        engine_sim::CheckedSolver::initializeSparseWorkspace(m_iv.J_sparse, coordinates, rows);
        engine_sim::CheckedSolver::initializeSparseWorkspace(m_iv.sreg0, coordinates, rows);
        m_solver.prepareFixedWorkspace(coordinates, rows);
        m_bodies = bodies;
        m_rows = rows;
        m_objects = getConstraintCount();
        m_generators = getForceGeneratorCount();
        m_prepared = true;
    }

    void process(double dt, int steps) override {
        engine_sim::requireProcessing(processSelected(dt, steps, nullptr));
    }

    engine_sim::ProcessingStatus processSelected(double dt, int steps, Engine *engine) {
        using Status = engine_sim::ProcessingStatus;
        assert(!m_prepared || (getRigidBodyCount() == m_bodies && getConstraintCount() == m_objects &&
            getFullConstraintCount() == m_rows && getForceGeneratorCount() == m_generators && steps == 1));
        const auto checkForces = [](void *context) noexcept {
            const auto &source = *static_cast<const Engine *>(context);
            for (int cylinder = 0; cylinder < source.getCylinderCount(); ++cylinder)
                if (source.getChamber(cylinder)->forceFailed()) return false;
            return true;
        };
        switch (processChecked(dt, steps, engine ? +checkForces : nullptr, engine)) {
        case ProcessResult::Ok: return Status::ok;
        case ProcessResult::Force: return Status::chamber_force;
        case ProcessResult::Solver: return Status::solver_convergence;
        }
        return Status::solver_convergence;
    }

    bool fixedWorkspaceStartsClear() {
        if (!m_prepared) return false;
        for (int row = 0; row < m_rows; ++row)
            if (m_iv.lambda.get(0, row) != 0.0) return false;
        return engine_sim::CheckedSolver::sparseWorkspaceStartsClear(m_iv.J_sparse, m_rows) &&
            engine_sim::CheckedSolver::sparseWorkspaceStartsClear(m_iv.sreg0, m_rows) &&
            m_solver.sparseWorkspaceStartsClear(m_rows);
    }

private:
    engine_sim::CheckedSolver m_solver;
    bool m_prepared = false;
    int m_bodies = 0, m_rows = 0, m_objects = 0, m_generators = 0;
};

class OwnedGenericSystem : public atg_scs::GenericRigidBodySystem {
public:
    OwnedGenericSystem() { initialize(&m_solver, &m_integrator); }
    ~OwnedGenericSystem() override {
        m_iv.J_sparse.destroy();
        m_iv.J_dot_sparse.destroy();
        m_iv.sreg0.destroy();
        m_iv.J_T.destroy();
        m_iv.M.destroy();
        m_iv.M_inv.destroy();
        m_iv.C.destroy();
        m_iv.ks.destroy();
        m_iv.kd.destroy();
        m_iv.q_dot.destroy();
        m_iv.reg0.destroy();
        m_iv.reg1.destroy();
        m_iv.reg2.destroy();
        m_iv.right.destroy();
        m_iv.F_ext.destroy();
        m_iv.F_C.destroy();
        m_iv.R.destroy();
        m_iv.lambda.destroy();
    }
private:
    atg_scs::GaussianEliminationSleSolver m_solver;
    atg_scs::NsvOdeSolver m_integrator;
};
}


Simulator::Simulator() {
    m_engine = nullptr;
    m_vehicle = nullptr;
    m_transmission = nullptr;
    m_system = nullptr;

    m_simulationSpeed = 1.0;
    m_simulationFrequency = 10000;
    m_steps = 0;

    m_currentIteration = 0;

    m_filteredEngineSpeed = 0.0;
    m_dynoTorqueSamples = nullptr;
    m_lastDynoTorqueSample = 0;
}

Simulator::~Simulator() {
    destroy();
}

void Simulator::initialize(const Parameters &params) {
    destroy();
    m_lastDynoTorqueSample = 0;
    m_filteredEngineSpeed = 0;
    if (params.systemType == SystemType::NsvOptimized) {
        m_system = new OwnedOptimizedSystem;
    }
    else {
        m_system = new OwnedGenericSystem;
    }

    m_dynoTorqueSamples = new double[DynoTorqueSamples];
    for (int i = 0; i < DynoTorqueSamples; ++i) {
        m_dynoTorqueSamples[i] = 0.0;
    }
}

void Simulator::loadSimulation(Engine *engine, Vehicle *vehicle, Transmission *transmission) {
    m_engine = engine;
    m_vehicle = vehicle;
    m_transmission = transmission;
}

void Simulator::releaseSimulation() {
    if (m_system != nullptr) m_system->reset();

    destroy();
}

void Simulator::prepareFixedWorkspace() {
    auto *system = dynamic_cast<OwnedOptimizedSystem *>(m_system);
    if (!system) throw engine_sim::SimulationFault("Fixed workspace requires the optimized solver");
    system->prepareFixedWorkspace();
}

bool Simulator::fixedWorkspaceStartsClear() {
    auto *system = dynamic_cast<OwnedOptimizedSystem *>(m_system);
    return system && system->fixedWorkspaceStartsClear();
}

std::size_t Simulator::acousticChannelCount() const {
    return m_engine == nullptr ? 0 : static_cast<std::size_t>(m_engine->getExhaustSystemCount());
}

bool Simulator::bodiesAreFinite() const noexcept {
    for (int i = 0; i < m_system->getRigidBodyCount(); ++i) {
        const auto &body = *m_system->getRigidBody(i);
        if (!std::isfinite(body.theta) || !std::isfinite(body.v_theta)
            || !std::isfinite(body.p_x) || !std::isfinite(body.p_y)
            || !std::isfinite(body.v_x) || !std::isfinite(body.v_y)) {
            return false;
        }
    }
    return true;
}

engine_sim::AcousticExcitation Simulator::advanceTick(std::size_t channelCapacity) {
    engine_sim::AcousticExcitation excitation;
    const auto status = advanceTickChecked(channelCapacity, excitation);
    if (status != engine_sim::ProcessingStatus::physics_no_progress) engine_sim::requireProcessing(status);
    return excitation;
}

engine_sim::ProcessingStatus Simulator::advanceTickChecked(std::size_t channelCapacity,
                                                            engine_sim::AcousticExcitation &excitation) {
    using Status = engine_sim::ProcessingStatus;
    excitation = {};
    const auto channels = acousticChannelCount();
    if (channels == 0 || channelCapacity < channels || !m_system) return Status::physics_no_progress;

    const double timestep = getTimestep();
    if (!bodiesAreFinite()) return Status::mechanical_state;
    if (auto *selected = dynamic_cast<OwnedOptimizedSystem *>(m_system)) {
        const auto status = selected->processSelected(timestep, 1, m_engine);
        if (status != Status::ok) return status;
    } else {
        m_system->process(timestep, 1);
    }
    if (!bodiesAreFinite()) return Status::mechanical_state;

    m_engine->update(timestep);
    m_vehicle->update(timestep);
    m_transmission->update(timestep);

    updateFilteredEngineSpeed(timestep);

    Crankshaft *outputShaft = m_engine->getOutputCrankshaft();
    outputShaft->resetAngle();

    for (int i = 0; i < m_engine->getCrankshaftCount(); ++i) {
        Crankshaft *shaft = m_engine->getCrankshaft(i);

        // Correct drift (temporary hack)
        shaft->m_body.theta = outputShaft->m_body.theta;
    }

    const double cycle = outputShaft->getCycleAngle();
    if (!std::isfinite(cycle) || cycle < 0 || cycle >= 4 * constants::pi) {
        return Status::crank_cycle;
    }
    const int index = static_cast<int>(std::floor(DynoTorqueSamples * cycle / (4 * constants::pi)));
    if (index < 0 || index >= DynoTorqueSamples) {
        return Status::torque_history;
    }
    const int step = m_engine->isSpinningCw() ? 1 : -1;
    m_dynoTorqueSamples[index] = m_dyno.getTorque();

    if (m_lastDynoTorqueSample != index) {
        for (int i = m_lastDynoTorqueSample + step; i != index; i += step) {
            if (i >= DynoTorqueSamples) {
                i = -1;
                continue;
            }
            else if (i < 0) {
                i = DynoTorqueSamples;
                continue;
            }

            m_dynoTorqueSamples[i] = m_dyno.getTorque();
        }

        m_lastDynoTorqueSample = index;
    }

    simulateStep_();
    return produceExhaustEndpointChecked(excitation);
}

double Simulator::getTotalExhaustFlow() const {
    return 0.0;
}

void Simulator::destroy() {
    if (m_system != nullptr) m_system->reset();
    delete m_system;
    m_system = nullptr;
    delete[] m_dynoTorqueSamples;
    m_dynoTorqueSamples = nullptr;
    m_engine = nullptr;
    m_vehicle = nullptr;
    m_transmission = nullptr;
    m_steps = m_currentIteration = 0;
}

double Simulator::getFilteredDynoTorque() const {
    if (m_dynoTorqueSamples == nullptr) return 0;

    double averageTorque = 0;
    for (int i = 0; i < DynoTorqueSamples; ++i) {
        averageTorque += m_dynoTorqueSamples[i];
    }

    return averageTorque / DynoTorqueSamples;
}

double Simulator::getDynoPower() const {
    return (m_engine != nullptr)
        ? getFilteredDynoTorque() * m_engine->getSpeed()
        : 0;
}

double Simulator::getAverageOutputSignal() const {
    return 0.0;
}

void Simulator::simulateStep_() {
}

void Simulator::updateFilteredEngineSpeed(double dt) {
    const double alpha = dt / (100 + dt);
    m_filteredEngineSpeed = alpha * m_filteredEngineSpeed + (1 - alpha) * m_engine->getRpm();
}
