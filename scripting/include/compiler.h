#ifndef ATG_ENGINE_SIM_COMPILER_H
#define ATG_ENGINE_SIM_COMPILER_H

#include "language_rules.h"

#include "engine_sim.h"
#include "../../include/engine_session.h"
#include "piranha.h"

#include <iosfwd>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace es_script {

    class Compiler {
    public:
        using Output = EngineDefinition;
        using Diagnostic = EngineLoadDiagnostic;
        using Error = EngineLoadError;
        const Diagnostic &getLastError() const { return m_lastError; }

    private:
        static thread_local Output *s_output;

    public:
        Compiler();
        ~Compiler();

        static Output *output();

        void initialize();
        void initialize(const std::vector<piranha::IrPath> &libraryRoots);
        bool compile(const piranha::IrPath &path);
        bool compile(const piranha::IrPath &path, std::ostream &diagnostics,
            std::vector<std::string> *resolvedScripts = nullptr);
        Output execute();
        void destroy();

    private:
        void printError(const piranha::CompilationError *err, std::ostream &diagnostics) const;
        void resetCompilation();

    private:
        LanguageRules m_rules;
        piranha::Compiler *m_compiler;
        std::unique_ptr<piranha::NodeProgram> m_program;
        std::vector<piranha::IrPath> m_libraryRoots;
        std::string m_entryPath;
        Diagnostic m_lastError;
        bool m_initialized = false;
        bool m_rulesInitialized = false;
        bool m_compiled = false;
    };

} /* namespace es_script */

#endif /* ATG_ENGINE_SIM_COMPILER_H */
