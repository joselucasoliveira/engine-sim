#ifndef ATG_ENGINE_SIM_FUNCTION_NODE_H
#define ATG_ENGINE_SIM_FUNCTION_NODE_H

#include "object_reference_node.h"

#include "engine_context.h"

#include "engine_sim.h"

namespace es_script {

    class FunctionNode : public ObjectReferenceNode<FunctionNode> {
    public:
        struct Sample {
            double x, y;
        };

    public:
        FunctionNode() { /* void */ }
        virtual ~FunctionNode() { /* void */ }

        void addSample(double x, double y) {
            requireFinite(x, "curve x");
            requireFinite(y, "curve y");
            m_samples.push_back({ x, y });
        }

        void setFilterRadius(double filterRadius) {
            m_filterRadius = filterRadius;
        }

        virtual Function *generate(EngineContext *context) {
            Function *existingFunction = context->getFunction(this);
            if (existingFunction != nullptr) {
                return existingFunction;
            }
            else {
                Function *function = context->getEngine()->createResource<Function>();
                function->initialize(modelCount(m_samples.size(), "curve sample count"), m_filterRadius);

                for (const Sample &sample : m_samples) {
                    function->addSample(sample.x, sample.y);
                }

                context->addFunction(this, function);
                return function;
            }
        }

    protected:
        virtual void registerInputs() {
            addInput("filter_radius", &m_filterRadius);

            ObjectReferenceNode<FunctionNode>::registerInputs();
        }

        virtual void _evaluate() {
            setOutput(this);

            // Read inputs
            readAllInputs();
        }

        std::vector<Sample> m_samples;
        double m_filterRadius = 0.0;
    };

} /* namespace es_script */

#endif /* ATG_ENGINE_SIM_FUNCTION_NODE_H */
