#ifndef ATG_ENGINE_SIM_SIMULATOR_H
#define ATG_ENGINE_SIM_SIMULATOR_H

#include "acoustic_excitation.h"
#include "simulation_fault.h"
#include "engine.h"
#include "transmission.h"
#include "vehicle.h"
#include "dynamometer.h"
#include "starter_motor.h"
#include "vehicle_drag_constraint.h"

namespace engine_sim_test { class LegacyFrame; }
namespace engine_sim { class PullEngine; }

class Simulator {
public:
    enum class SystemType {
        NsvOptimized,
        Generic
    };

    struct Parameters {
        SystemType systemType = SystemType::NsvOptimized;
    };

    static constexpr int DynoTorqueSamples = 512;

public:
    Simulator();
    virtual ~Simulator();

    virtual void initialize(const Parameters &params);
    void loadSimulation(Engine *engine, Vehicle *vehicle, Transmission *transmission);
    void releaseSimulation();
    // Used once by a detached, final public graph before it is published.
    // Allocates and initializes the solver workspace without evaluating a step.
    void prepareFixedWorkspace();
    bool fixedWorkspaceStartsClear();

    // Run one mechanical/fluid tick. An extent smaller than the prepared
    // channel count returns an empty view without advancing any state.
    // The view remains valid until the next tick or teardown.
    engine_sim::AcousticExcitation advanceTick(std::size_t channelCapacity);
    engine_sim::ProcessingStatus advanceTickChecked(std::size_t channelCapacity,
                                                     engine_sim::AcousticExcitation &excitation);
    std::size_t acousticChannelCount() const;
    virtual double getTotalExhaustFlow() const;
    virtual void destroy();

    int getFrameIterationCount() const { return m_steps; }

    Engine *getEngine() { return m_engine; }
    const Engine *getEngine() const { return m_engine; }
    Transmission *getTransmission() { return m_transmission; }
    const Transmission *getTransmission() const { return m_transmission; }
    Vehicle *getVehicle() { return m_vehicle; }
    const Vehicle *getVehicle() const { return m_vehicle; }
    atg_scs::RigidBodySystem *getSystem() { return m_system; }

    void setSimulationFrequency(int frequency) { m_simulationFrequency = frequency; }
    int getSimulationFrequency() const { return m_simulationFrequency; }

    double getTimestep() const { return 1.0 / m_simulationFrequency; }

    void setSimulationSpeed(double simSpeed) { m_simulationSpeed = simSpeed; }
    double getSimulationSpeed() const { return m_simulationSpeed; }
    int getCurrentIteration() const { return m_currentIteration; }

    virtual double getFilteredDynoTorque() const;
    virtual double getDynoPower() const;
    virtual double getAverageOutputSignal() const;

    double filteredEngineSpeed() const { return m_filteredEngineSpeed; }

    Dynamometer m_dyno;
    StarterMotor m_starterMotor;

protected:
    virtual void simulateStep_();
    virtual engine_sim::AcousticExcitation produceExhaustEndpoint() = 0;
    virtual engine_sim::ProcessingStatus produceExhaustEndpointChecked(engine_sim::AcousticExcitation &excitation) {
        excitation = produceExhaustEndpoint();
        return engine_sim::ProcessingStatus::ok;
    }

    atg_scs::RigidBodySystem *m_system;

private:
    friend class engine_sim_test::LegacyFrame;
    friend class engine_sim::PullEngine;
    bool bodiesAreFinite() const noexcept;
    void updateFilteredEngineSpeed(double dt);

private:
    atg_scs::RigidBody m_vehicleMass;
    VehicleDragConstraint m_vehicleDrag;

    int m_currentIteration;

    Engine *m_engine;
    Transmission *m_transmission;
    Vehicle *m_vehicle;

    int m_simulationFrequency;

    double m_simulationSpeed;

    double *m_dynoTorqueSamples;
    int m_lastDynoTorqueSample;

    double m_filteredEngineSpeed;

    int m_steps;
};

#endif /* ATG_ENGINE_SIM_SIMULATOR_H */
