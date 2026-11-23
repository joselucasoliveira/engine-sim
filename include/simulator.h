#ifndef ATG_ENGINE_SIM_SIMULATOR_H
#define ATG_ENGINE_SIM_SIMULATOR_H

#include "acoustic_excitation.h"
#include "engine.h"
#include "transmission.h"
#include "vehicle.h"
#include "synthesizer.h"
#include "dynamometer.h"
#include "starter_motor.h"
#include "derivative_filter.h"
#include "vehicle_drag_constraint.h"
#include "delay_filter.h"
#include "engine.h"

#include <chrono>
#include <vector>

namespace engine_sim_test { struct LegacyAccess; }
namespace engine_sim { class PullEngine; }
class EngineSession;

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

    virtual void startFrame(double dt);
    bool simulateStep();
    virtual double getTotalExhaustFlow() const;
    int readAudioOutput(int samples, int16_t *target);
    virtual void endFrame();
    virtual void destroy();

    void startAudioRenderingThread();
    void endAudioRenderingThread();

    int getFrameIterationCount() const { return m_steps; }

    // Temporary legacy host/test forwarding. The session owns synthesis.
    Synthesizer &synthesizer();
    const Synthesizer &synthesizer() const;

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

    void setTargetSynthesizerLatency(double latency) { m_targetSynthesizerLatency = latency; }
    double getTargetSynthesizerLatency() const { return m_targetSynthesizerLatency; }
    double getSynthesizerInputLatency() const { return synthesizer().getLatency(); }
    double getSynthesizerInputLatencyTarget() const;

    void setSimulationSpeed(double simSpeed) { m_simulationSpeed = simSpeed; }
    double getSimulationSpeed() const { return m_simulationSpeed; }
    int getCurrentIteration() const { return m_currentIteration; }
    double getAverageProcessingTime() const { return m_physicsProcessingTime; }

    int simulationSteps() const { return m_steps; }

    virtual double getFilteredDynoTorque() const;
    virtual double getDynoPower() const;
    virtual double getAverageOutputSignal() const;

    double filteredEngineSpeed() const { return m_filteredEngineSpeed; }

    Dynamometer m_dyno;
    StarterMotor m_starterMotor;

protected:
    virtual void simulateStep_();
    virtual engine_sim::AcousticExcitation produceExhaustEndpoint() = 0;

    atg_scs::RigidBodySystem *m_system;

private:
    friend struct engine_sim_test::LegacyAccess;
    friend class engine_sim::PullEngine;
    friend class EngineSession;
    engine_sim::AcousticExcitation advanceTick(bool legacyFrame);
    void validateBodies() const;
    bool beginFrame();
    void resetFrameFlows();
    void updateFilteredEngineSpeed(double dt);

private:
    atg_scs::RigidBody m_vehicleMass;
    VehicleDragConstraint m_vehicleDrag;

    Synthesizer *m_legacySynthesis = nullptr;

    std::chrono::steady_clock::time_point m_simulationStart;
    std::chrono::steady_clock::time_point m_simulationEnd;
    int m_currentIteration;

    Engine *m_engine;
    Transmission *m_transmission;
    Vehicle *m_vehicle;

    double m_physicsProcessingTime;

    int m_simulationFrequency;

    double m_targetSynthesizerLatency;
    double m_simulationSpeed;

    double *m_dynoTorqueSamples;
    int m_lastDynoTorqueSample;

    double m_filteredEngineSpeed;

    int m_steps;
    std::vector<double> m_frameIntakeFlows;
};

#endif /* ATG_ENGINE_SIM_SIMULATOR_H */
