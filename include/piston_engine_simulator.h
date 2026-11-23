#ifndef ATG_ENGINE_SIM_PISTON_ENGINE_SIMULATOR_H
#define ATG_ENGINE_SIM_PISTON_ENGINE_SIMULATOR_H

#include "simulator.h"

#include "engine.h"
#include "transmission.h"
#include "combustion_chamber.h"
#include "vehicle.h"
#include "dynamometer.h"
#include "starter_motor.h"
#include "vehicle_drag_constraint.h"
#include "delay_filter.h"

#include "scs.h"

namespace engine_sim::assembly { struct CapacityInspection; }

class PistonEngineSimulator : public Simulator {
    public:
        PistonEngineSimulator();
        virtual ~PistonEngineSimulator() override;

        void loadSimulation(Engine *engine, Vehicle *vehicle, Transmission *transmission);

        virtual double getTotalExhaustFlow() const;
        virtual void destroy() override;

        void setFluidSimulationSteps(int steps) { m_fluidSimulationSteps = steps; }
        int getFluidSimulationSteps() const { return m_fluidSimulationSteps; }
        int getFluidSimulationFrequency() const { return m_fluidSimulationSteps * getSimulationFrequency(); }

        virtual double getAverageOutputSignal() const override;

        // Public runtime brake, separate from inherent model friction and dyno.
        // Opt in after preparation so legacy/desktop graph ordering is unchanged.
        void enableExternalLoad();
        void setExternalLoadNm(double torque) noexcept;
        double getExternalLoadReactionNm() const noexcept;

    protected:
        virtual void simulateStep_() override;

    protected:
        void placeAndInitialize();
        void placeCylinder(int i);
        
    protected:
        engine_sim::AcousticExcitation produceExhaustEndpoint() override;
        engine_sim::ProcessingStatus produceExhaustEndpointChecked(engine_sim::AcousticExcitation &excitation) override;

    protected:
        friend struct engine_sim::assembly::CapacityInspection;
        DelayFilter *m_delayFilters;

        atg_scs::FixedPositionConstraint *m_crankConstraints;
        atg_scs::ClutchConstraint *m_crankshaftLinks;
        atg_scs::RotationFrictionConstraint *m_crankshaftFrictionConstraints;
        atg_scs::RotationFrictionConstraint m_externalLoad;
        bool m_externalLoadEnabled = false;
        atg_scs::LineConstraint *m_cylinderWallConstraints;
        atg_scs::LinkConstraint *m_linkConstraints;
        atg_scs::RigidBody m_vehicleMass;
        VehicleDragConstraint m_vehicleDrag;

        Engine *m_engine;
        Transmission *m_transmission;
        Vehicle *m_vehicle;

        double *m_exhaustFlowStagingBuffer;

        int m_fluidSimulationSteps;
};

#endif /* ATG_ENGINE_SIM_PISTON_ENGINE_SIMULATOR_H */
