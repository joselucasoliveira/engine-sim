#include "../include/simulator.h"
#include "../include/checked_solver.h"

#include <cmath>

namespace {
class OwnedOptimizedSystem : public atg_scs::OptimizedNsvRigidBodySystem {
public:
    OwnedOptimizedSystem() { initialize(&m_solver); }
private:
    engine_sim::CheckedSolver m_solver;
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

std::size_t Simulator::acousticChannelCount() const {
    return m_engine == nullptr ? 0 : static_cast<std::size_t>(m_engine->getExhaustSystemCount());
}

void Simulator::validateBodies() const {
    for (int i = 0; i < m_system->getRigidBodyCount(); ++i) {
        const auto &body = *m_system->getRigidBody(i);
        if (!std::isfinite(body.theta) || !std::isfinite(body.v_theta)
            || !std::isfinite(body.p_x) || !std::isfinite(body.p_y)
            || !std::isfinite(body.v_x) || !std::isfinite(body.v_y)) {
            throw engine_sim::SimulationFault("nonfinite mechanical state");
        }
    }
}

engine_sim::AcousticExcitation Simulator::advanceTick(std::size_t channelCapacity) {
    const auto channels = acousticChannelCount();
    if (channels == 0 || channelCapacity < channels) return {};

    const double timestep = getTimestep();
    validateBodies();
    m_system->process(timestep, 1);
    validateBodies();

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
        throw engine_sim::SimulationFault("invalid crank cycle angle");
    }
    const int index = static_cast<int>(std::floor(DynoTorqueSamples * cycle / (4 * constants::pi)));
    if (index < 0 || index >= DynoTorqueSamples) {
        throw engine_sim::SimulationFault("crank cycle index outside torque history");
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
    return produceExhaustEndpoint();
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
