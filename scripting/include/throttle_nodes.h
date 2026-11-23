#ifndef ATG_ENGINE_SIM_THROTTLE_NODES_H
#define ATG_ENGINE_SIM_THROTTLE_NODES_H

#include "object_reference_node.h"

#include "engine_sim.h"

namespace es_script {

    class ThrottleNode : public ObjectReferenceNode<ThrottleNode> {
    public:
        ThrottleNode() { /* void */ }
        virtual ~ThrottleNode() { /* void */ }

        virtual Throttle *generate() const = 0;

    protected:
        virtual void _evaluate() {
            setOutput(this);
            readAllInputs();
        }
    };

    class DirectThrottleLinkageNode : public ThrottleNode {
        friend class LegacySnapshotBuilder;

    public:
        DirectThrottleLinkageNode() { /* void */ }
        virtual ~DirectThrottleLinkageNode() { /* void */ }

        virtual Throttle *generate() const override {
            DirectThrottleLinkage *throttle = new DirectThrottleLinkage;
            throttle->initialize(m_parameters);

            return static_cast<Throttle *>(throttle);
        }

        void validate() const override { requirePositive(m_parameters.gamma, "throttle gamma"); }

    protected:
        virtual void registerInputs() override {
            addInput("gamma", &m_parameters.gamma);

            ThrottleNode::registerInputs();
        }

        DirectThrottleLinkage::Parameters m_parameters;
    };

    class GovernorNode : public ThrottleNode {
        friend class LegacySnapshotBuilder;

    public:
        GovernorNode() { /* void */ }
        virtual ~GovernorNode() { /* void */ }

        virtual Throttle *generate() const override {
            Governor *throttle = new Governor;
            throttle->initialize(m_parameters);

            return static_cast<Throttle *>(throttle);
        }

        void validate() const override {
            requirePositive(m_parameters.gamma, "governor gamma");
            requireNonnegative(m_parameters.minSpeed, "governor minimum speed");
            requireModel(m_parameters.maxSpeed >= m_parameters.minSpeed, "Governor speed range is reversed");
            requireModel(m_parameters.maxVelocity >= m_parameters.minVelocity, "Governor velocity range is reversed");
            requireNonnegative(m_parameters.k_s, "governor spring coefficient");
            requireNonnegative(m_parameters.k_d, "governor damping coefficient");
        }

    protected:
        virtual void registerInputs() override {
            addInput("min_speed", &m_parameters.minSpeed);
            addInput("max_speed", &m_parameters.maxSpeed);
            addInput("min_v", &m_parameters.minVelocity);
            addInput("max_v", &m_parameters.maxVelocity);
            addInput("k_s", &m_parameters.k_s);
            addInput("k_d", &m_parameters.k_d);
            addInput("gamma", &m_parameters.gamma);

            ThrottleNode::registerInputs();
        }

        Governor::Parameters m_parameters;
    };

} /* namespace es_script */

#endif /* ATG_ENGINE_SIM_THROTTLE_NODES_H */
