#ifndef ATG_ENGINE_SIM_NODE_H
#define ATG_ENGINE_SIM_NODE_H

#include "piranha.h"
#include "model_validation.h"

#include <map>
#include <string>
#include <type_traits>

namespace es_script {

    class Node : public piranha::Node {
    protected:
        struct InputTarget {
            enum class Type {
                Object,
                Atomic
            };

            piranha::pNodeInput *input = nullptr;
            void *memoryTarget = nullptr;
            Type type = Type::Atomic;
            bool (*valid)(const void *) = nullptr;
        };

    public:
        Node() {
            /* void */
        }

        virtual ~Node() {
            for (auto i : m_inputMap) {
                delete i.second.input;
            }
        }

        template <typename T_Out>
        T_Out readAtomicInput(const std::string &name) {
            const auto found = m_inputMap.find(name);
            requireModel(found != m_inputMap.end() && found->second.input != nullptr
                && *found->second.input != nullptr, "Missing input: " + name);
            T_Out out{};
            (*found->second.input)->fullCompute(&out);
            checkRuntimeError();
            if constexpr (std::is_floating_point_v<T_Out>) requireFinite(out, name);
            return out;
        }

        void readAllInputs() {
            for (auto i : m_inputMap) {
                if (i.second.type == InputTarget::Type::Atomic
                    || i.second.type == InputTarget::Type::Object)
                {
                    requireModel(i.second.input != nullptr && *i.second.input != nullptr,
                        "Missing input: " + i.first);
                    (*i.second.input)->fullCompute(i.second.memoryTarget);
                    checkRuntimeError();
                    requireModel(i.second.valid == nullptr || i.second.valid(i.second.memoryTarget),
                        "Input must be finite: " + i.first);
                }
            }
        }

        template <typename T>
        void addInput(
            const std::string &name,
            T *target,
            InputTarget::Type type = InputTarget::Type::Atomic)
        {
            m_inputMap[name] = {
                new piranha::pNodeInput{},
                target,
                type,
                [](const void *value) {
                    if constexpr (std::is_floating_point_v<T>) return std::isfinite(*static_cast<const T *>(value));
                    else return true;
                }
            };
        }

        virtual void registerInputs() {
            for (auto i : m_inputMap) {
                registerInput(i.second.input, i.first);
            }
        }

    private:
        void checkRuntimeError() {
            if (getProgram() != nullptr) {
                const auto message = getProgram()->getRuntimeError();
                if (!message.empty()) throw ModelError(message);
            }
        }
        std::map<std::string, InputTarget> m_inputMap;
    };

} /* namespace es_script */

#endif /* ATG_ENGINE_SIM_NODE_H */
