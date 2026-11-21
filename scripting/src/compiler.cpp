#include "../include/compiler.h"

#include <iostream>
#include "../include/model_validation.h"
#include "../../include/resource_paths.h"
#include <set>

#include <piranha-portable/include/ir_import_statement.h>

namespace {
void collectScripts(const piranha::IrCompilationUnit &unit,
        std::set<const piranha::IrCompilationUnit *> &visited, std::vector<std::string> &paths) {
    if (!visited.insert(&unit).second) return;
    paths.push_back(unit.getPath().toString());
    for (int i = 0; i < unit.getImportStatementCount(); ++i) {
        const auto *dependency = unit.getImportStatement(i)->getUnit();
        if (dependency != nullptr) collectScripts(*dependency, visited, paths);
    }
}
}

thread_local es_script::Compiler::Output *es_script::Compiler::s_output = nullptr;

es_script::Compiler::Compiler() : m_compiler(nullptr) {}

es_script::Compiler::~Compiler() { destroy(); }

es_script::Compiler::Output *es_script::Compiler::output() {
    if (s_output == nullptr) throw std::logic_error("No active script execution output");
    return s_output;
}

void es_script::Compiler::initialize() {
    const auto paths = engine_sim::ResourcePaths::resolve(engine_sim::executablePath(), {});
    initialize({paths.library.u8string(), paths.assets.u8string()});
}

void es_script::Compiler::initialize(const std::vector<piranha::IrPath> &libraryRoots) {
    resetCompilation();
    m_libraryRoots = libraryRoots;
    if (!m_rulesInitialized) {
        m_rules.initialize();
        m_rulesInitialized = true;
    }
    m_initialized = true;
}

bool es_script::Compiler::compile(const piranha::IrPath &path) { return compile(path, std::cerr); }

bool es_script::Compiler::compile(const piranha::IrPath &path, std::ostream &diagnostics,
        std::vector<std::string> *resolvedScripts) {
    if (resolvedScripts != nullptr) resolvedScripts->clear();
    resetCompilation();
    m_entryPath = path.toString();
    m_lastError = {};
    if (!m_initialized) {
        m_lastError = {"compile", m_entryPath, "Compiler is not initialized"};
        diagnostics << m_lastError.format() << '\n';
        return false;
    }
    try {
        m_compiler = new piranha::Compiler(&m_rules);
        m_compiler->setFileExtension(".mr");
        for (const auto &root : m_libraryRoots) m_compiler->addSearchPath(root);
        m_program = std::make_unique<piranha::NodeProgram>();
        piranha::IrCompilationUnit *unit = m_compiler->compile(path);
        if (unit == nullptr) {
            m_lastError = {"compile", m_entryPath, "Can't find file"};
            diagnostics << "Can't find file: " << m_entryPath << "\n";
            return false;
        }
        const auto *errors = m_compiler->getErrorList();
        if (errors->getErrorCount() != 0) {
            m_lastError = {"compile", m_entryPath, "Script contains compilation errors"};
            for (int i = 0; i < errors->getErrorCount(); ++i) printError(errors->getCompilationError(i), diagnostics);
            return false;
        }
        if (resolvedScripts != nullptr) {
            std::set<const piranha::IrCompilationUnit *> visited;
            collectScripts(*unit, visited, *resolvedScripts);
        }
        unit->build(m_program.get());
        m_program->initialize();
        m_compiled = true;
        return true;
    }
    catch (const std::exception &error) {
        m_lastError = {"compile", m_entryPath, error.what()};
        diagnostics << m_lastError.format() << '\n';
        return false;
    }
}

es_script::Compiler::Output es_script::Compiler::execute() {
    if (!m_compiled) {
        m_lastError = {"execute", m_entryPath, "No unexecuted successful compilation"};
        throw Error(m_lastError);
    }
    m_compiled = false;
    Output pending;
    struct ActiveOutput {
        Output *&slot;
        Output *previous;
        ~ActiveOutput() { slot = previous; }
    } active{s_output, s_output};
    s_output = &pending;
    try {
        pending.sourcePath = m_entryPath;
        if (!m_program->execute()) {
            m_lastError = {"execute", m_entryPath, m_program->getRuntimeError()};
            throw Error(m_lastError);
        }
        requireModel(pending.engine != nullptr, "Script did not select an engine");
        return pending;
    }
    catch (const Error &) { throw; }
    catch (const ModelError &error) {
        const auto runtime = m_program->getRuntimeError();
        m_lastError = {runtime.empty() ? "graph" : "execute", m_entryPath, runtime.empty() ? error.what() : runtime};
        throw Error(m_lastError);
    }
    catch (const std::exception &error) {
        m_lastError = {"execute", m_entryPath, error.what()};
        throw Error(m_lastError);
    }
}

void es_script::Compiler::resetCompilation() {
    m_compiled = false;
    if (m_program != nullptr) { m_program->free(); m_program.reset(); }
    if (m_compiler != nullptr) { m_compiler->free(); delete m_compiler; m_compiler = nullptr; }
}

void es_script::Compiler::destroy() {
    resetCompilation();
    m_initialized = false;
}

void es_script::Compiler::printError(
    const piranha::CompilationError *err,
    std::ostream &diagnostics) const
{
    const piranha::ErrorCode_struct &errorCode = err->getErrorCode();
    diagnostics << err->getCompilationUnit()->getPath().getStem()
        << "(" << err->getErrorLocation()->lineStart << "): error "
        << errorCode.stage << errorCode.code << ": " << errorCode.info << std::endl;

    piranha::IrContextTree *context = err->getInstantiation();
    while (context != nullptr) {
        piranha::IrNode *instance = context->getContext();
        if (instance != nullptr) {
            const std::string instanceName = instance->getName();
            const std::string definitionName = (instance->getDefinition() != nullptr)
                ? instance->getDefinition()->getName()
                : "<Type Error>";
            const std::string formattedName = (instanceName.empty())
                ? "<unnamed> " + definitionName
                : instanceName + " " + definitionName;

            diagnostics
                << "       While instantiating: "
                << instance->getParentUnit()->getPath().getStem()
                << "(" << instance->getSummaryToken()->lineStart << "): "
                << formattedName << std::endl;
        }

        context = context->getParent();
    }
}
