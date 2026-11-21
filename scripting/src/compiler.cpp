#include "../include/compiler.h"

#include <fstream>
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

es_script::Compiler::Output *es_script::Compiler::s_output = nullptr;

es_script::Compiler::Compiler() {
    m_compiler = nullptr;
}

es_script::Compiler::~Compiler() {
    assert(m_compiler == nullptr);
}

es_script::Compiler::Output *es_script::Compiler::output() {
    if (s_output == nullptr) {
        s_output = new Output;
    }

    return s_output;
}

void es_script::Compiler::initialize() {
    initialize({"../../es/", "../es/", "es/"});
}

void es_script::Compiler::initialize(const std::vector<piranha::IrPath> &libraryRoots) {
    m_compiler = new piranha::Compiler(&m_rules);
    m_compiler->setFileExtension(".mr");
    for (const auto &root : libraryRoots) {
        m_compiler->addSearchPath(root);
    }

    m_rules.initialize();
}

bool es_script::Compiler::compile(const piranha::IrPath &path) {
    std::ofstream diagnostics("error_log.log", std::ios::out);
    return compile(path, diagnostics);
}

bool es_script::Compiler::compile(const piranha::IrPath &path, std::ostream &diagnostics,
        std::vector<std::string> *resolvedScripts) {
    if (resolvedScripts != nullptr) resolvedScripts->clear();
    bool successful = false;
    piranha::IrCompilationUnit *unit = m_compiler->compile(path);
    if (unit == nullptr) {
        diagnostics << "Can't find file: " << path.toString() << "\n";
    }
    else {
        const piranha::ErrorList *errors = m_compiler->getErrorList();
        if (errors->getErrorCount() == 0) {
            if (resolvedScripts != nullptr) {
                std::set<const piranha::IrCompilationUnit *> visited;
                collectScripts(*unit, visited, *resolvedScripts);
            }
            unit->build(&m_program);

            m_program.initialize();

            successful = true;
        }
        else {
            for (int i = 0; i < errors->getErrorCount(); ++i) {
                printError(errors->getCompilationError(i), diagnostics);
            }
        }
    }

    return successful;
}

es_script::Compiler::Output es_script::Compiler::execute() {
    const bool result = m_program.execute();

    if (!result) {
        // Todo: Runtime error
    }

    return *output();
}

void es_script::Compiler::destroy() {
    m_program.free();
    m_compiler->free();

    delete m_compiler;
    m_compiler = nullptr;
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
