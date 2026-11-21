#ifndef ATG_ENGINE_SIM_VTEC_VALVETRAIN_NODE_H
#define ATG_ENGINE_SIM_VTEC_VALVETRAIN_NODE_H

#include "valvetrain_node.h"

#include "camshaft_node.h"

#include "engine_sim.h"

namespace es_script {

    class VtecValvetrainNode : public ValvetrainNode {
    public:
        VtecValvetrainNode() { /* void */ }
        virtual ~VtecValvetrainNode() { /* void */ }

        virtual Valvetrain *generate(
            EngineContext *context,
            Crankshaft *crank) override {
            VtecValvetrain *valvetrain = context->getEngine()->createResource<VtecValvetrain>();

            Camshaft
                *intakeCam = context->getEngine()->createResource<Camshaft>(),
                *exhaustCam = context->getEngine()->createResource<Camshaft>(),
                *vtecIntakeCam = context->getEngine()->createResource<Camshaft>(),
                *vtecExhaustCam = context->getEngine()->createResource<Camshaft>();

            m_intakeCamshaft->generate(intakeCam, crank, context);
            m_exhaustCamshaft->generate(exhaustCam, crank, context);
            m_vtecIntakeCamshaft->generate(vtecIntakeCam, crank, context);
            m_vtecExhaustCamshaft->generate(vtecExhaustCam, crank, context);

            VtecValvetrain::Parameters params;
            params.intakeCamshaft = intakeCam;
            params.exhaustCamshaft = exhaustCam;
            params.vtecIntakeCamshaft = vtecIntakeCam;
            params.vtexExhaustCamshaft = vtecExhaustCam;
            params.minRpm = m_parameters.minRpm;
            params.minSpeed = m_parameters.minSpeed;
            params.minThrottlePosition = m_parameters.minThrottlePosition;
            params.manifoldVacuum = m_parameters.manifoldVacuum;
            params.engine = context->getEngine();
            valvetrain->initialize(params);

            return valvetrain;
        }

        void validate() const override {
            requireModel(m_intakeCamshaft != nullptr, "Missing camshaft: intakeCamshaft");
            m_intakeCamshaft->validate();
            requireModel(m_exhaustCamshaft != nullptr, "Missing camshaft: exhaustCamshaft");
            m_exhaustCamshaft->validate();
            requireModel(m_vtecIntakeCamshaft != nullptr, "Missing camshaft: vtecIntakeCamshaft");
            m_vtecIntakeCamshaft->validate();
            requireModel(m_vtecExhaustCamshaft != nullptr, "Missing camshaft: vtecExhaustCamshaft");
            m_vtecExhaustCamshaft->validate();
        }
        void validateCylinderCount(int count) const override {
            validate();
            requireModel(m_intakeCamshaft->getLobeCount() >= count, "Cam has fewer lobes than cylinders");
            requireModel(m_exhaustCamshaft->getLobeCount() >= count, "Cam has fewer lobes than cylinders");
            requireModel(m_vtecIntakeCamshaft->getLobeCount() >= count, "Cam has fewer lobes than cylinders");
            requireModel(m_vtecExhaustCamshaft->getLobeCount() >= count, "Cam has fewer lobes than cylinders");
        }

    protected:
        virtual void registerInputs() {
            addInput("vtec_intake_camshaft", &m_vtecIntakeCamshaft, InputTarget::Type::Object);
            addInput("vtec_exhaust_camshaft", &m_vtecExhaustCamshaft, InputTarget::Type::Object);
            addInput("intake_camshaft", &m_intakeCamshaft, InputTarget::Type::Object);
            addInput("exhaust_camshaft", &m_exhaustCamshaft, InputTarget::Type::Object);

            addInput("min_rpm", &m_parameters.minRpm);
            addInput("min_speed", &m_parameters.minSpeed);
            addInput("manifold_vacuum", &m_parameters.manifoldVacuum);
            addInput("min_throttle_position", &m_parameters.minThrottlePosition);

            ValvetrainNode::registerInputs();
        }

    private:
        CamshaftNode *m_vtecIntakeCamshaft = nullptr;
        CamshaftNode *m_vtecExhaustCamshaft = nullptr;
        CamshaftNode *m_intakeCamshaft = nullptr;
        CamshaftNode *m_exhaustCamshaft = nullptr;

        VtecValvetrain::Parameters m_parameters;
    };

} /* namespace es_script */

#endif /* ATG_ENGINE_SIM_VTEC_VALVETRAIN_NODE_H */
