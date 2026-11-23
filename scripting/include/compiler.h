#ifndef ATG_ENGINE_SIM_COMPILER_H
#define ATG_ENGINE_SIM_COMPILER_H

#include "language_rules.h"

#include "engine_sim.h"
#include "../../include/engine_session.h"
#include "piranha.h"

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace es_script {

    struct PreparationContext {
        std::filesystem::path entry, libraryRoot, assetRoot;
    };

    struct ScriptInput {
        std::filesystem::path resolvedPath;
        std::string bytes;
    };

    class EngineNode;
    class CylinderBankNode;
    class IgnitionWireNode;
    class TransmissionNode;
    class VehicleNode;

    class EvaluationObserver {
    public:
        virtual ~EvaluationObserver() = default;
        virtual void selectedEngine(const EngineNode &engine) = 0;
        virtual void addedCylinder(const CylinderBankNode &bank, const IgnitionWireNode &wire) = 0;
        virtual void selectedVehicle(const VehicleNode &vehicle) = 0;
        virtual void selectedTransmission(const TransmissionNode &transmission) = 0;
        virtual void selectedSettings(const ApplicationSettings &settings) = 0;
    };

    class Compiler {
    public:
        using Output = EngineDefinition;
        using Diagnostic = EngineLoadDiagnostic;
        using Error = EngineLoadError;
        using InputVerifier = std::function<void(const ScriptInput &)>;
        const Diagnostic &getLastError() const { return m_lastError; }

    private:
        struct ActiveEvaluation {
            Output &output;
            EvaluationObserver *observer;
        };
        static thread_local ActiveEvaluation *s_evaluation;

    public:
        Compiler();
        ~Compiler();

        static Output *output();
        static EvaluationObserver *observer() noexcept;
        static Output prepare(const PreparationContext &context, std::vector<ScriptInput> &inputs,
            const InputVerifier &verify = {}, EvaluationObserver *observer = nullptr);

        void initialize(const std::vector<std::string> &libraryRoots);
        bool compile(const std::string &path, std::ostream &diagnostics,
            std::vector<std::string> *resolvedScripts = nullptr);
        Output execute(EvaluationObserver *observer = nullptr);
        void destroy();

    private:
        void printError(const piranha::CompilationError *err, std::ostream &diagnostics) const;
        void resetCompilation();

    private:
        std::unique_lock<std::recursive_mutex> m_preparation;
        std::unique_ptr<LanguageRules> m_rules;
        piranha::Compiler *m_compiler;
        std::unique_ptr<piranha::NodeProgram> m_program;
        piranha::Compiler::SourceObserver m_sourceObserver;
        std::vector<std::string> m_libraryRoots;
        std::string m_entryPath;
        Diagnostic m_lastError;
        bool m_resourceFailure = false;
        bool m_compiled = false;
    };

} /* namespace es_script */

#endif /* ATG_ENGINE_SIM_COMPILER_H */
