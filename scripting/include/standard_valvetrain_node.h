#ifndef ATG_ENGINE_SIM_STANDARD_VALVETRAIN_NODE_H
#define ATG_ENGINE_SIM_STANDARD_VALVETRAIN_NODE_H

#include "valvetrain_node.h"

#include "camshaft_node.h"

#include "engine_sim.h"

namespace es_script {

    class StandardValvetrainNode : public ValvetrainNode {
        friend class LegacySnapshotBuilder;

    public:
        StandardValvetrainNode() { /* void */ }
        virtual ~StandardValvetrainNode() { /* void */ }

        virtual Valvetrain *generate(
            EngineContext *context,
            Crankshaft *crank) override
        {
            StandardValvetrain *valvetrain = context->getEngine()->createResource<StandardValvetrain>();

            Camshaft
                *intakeCam = context->getEngine()->createResource<Camshaft>(),
                *exhaustCam = context->getEngine()->createResource<Camshaft>();

            m_intakeCamshaft->generate(intakeCam, crank, context);
            m_exhaustCamshaft->generate(exhaustCam, crank, context);

            StandardValvetrain::Parameters params;
            params.intakeCamshaft = intakeCam;
            params.exhaustCamshaft = exhaustCam;
            valvetrain->initialize(params);

            return valvetrain;
        }

        void validate() const override {
            requireModel(m_intakeCamshaft != nullptr, "Missing camshaft: intakeCamshaft");
            m_intakeCamshaft->validate();
            requireModel(m_exhaustCamshaft != nullptr, "Missing camshaft: exhaustCamshaft");
            m_exhaustCamshaft->validate();
        }
        void validateCylinderCount(int count) const override {
            validate();
            requireModel(m_intakeCamshaft->getLobeCount() >= count, "Cam has fewer lobes than cylinders");
            requireModel(m_exhaustCamshaft->getLobeCount() >= count, "Cam has fewer lobes than cylinders");
        }

    protected:
        virtual void registerInputs() {
            addInput("intake_camshaft", &m_intakeCamshaft, InputTarget::Type::Object);
            addInput("exhaust_camshaft", &m_exhaustCamshaft, InputTarget::Type::Object);

            ValvetrainNode::registerInputs();
        }

    private:
        CamshaftNode *m_intakeCamshaft = nullptr;
        CamshaftNode *m_exhaustCamshaft = nullptr;
    };

} /* namespace es_script */

#endif /* ATG_ENGINE_SIM_STANDARD_VALVETRAIN_NODE_H */
