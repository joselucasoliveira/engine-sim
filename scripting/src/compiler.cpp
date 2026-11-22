#include "../include/compiler.h"

#include "../include/model_validation.h"
#include <cstdint>
#include <ostream>
#include <set>
#include <sstream>
#include <string_view>

#include <piranha-portable/include/ir_import_statement.h>

namespace {
std::recursive_mutex &preparationMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

namespace fs = std::filesystem;

void requireUtf8(std::string_view bytes) {
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto lead = static_cast<unsigned char>(bytes[offset++]);
        if (lead > 0 && lead < 0x80) continue;
        const unsigned count = lead >= 0xc2 && lead <= 0xdf ? 1 :
            lead >= 0xe0 && lead <= 0xef ? 2 : lead >= 0xf0 && lead <= 0xf4 ? 3 : 0;
        if (count == 0 || count > bytes.size() - offset) throw std::invalid_argument("Expected valid UTF-8");
        std::uint32_t point = lead & ((1u << (6 - count)) - 1);
        for (unsigned next = 0; next < count; ++next) {
            const auto byte = static_cast<unsigned char>(bytes[offset++]);
            if ((byte & 0xc0) != 0x80) throw std::invalid_argument("Expected valid UTF-8");
            point = (point << 6) | (byte & 0x3f);
        }
        const std::uint32_t minimum[] = {0, 0x80, 0x800, 0x10000};
        if (point < minimum[count] || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) {
            throw std::invalid_argument("Expected valid UTF-8");
        }
    }
}

bool isFileOpenError(const piranha::CompilationError &error) {
    const auto &code = error.getErrorCode();
    return code.stage == piranha::ErrorCode::FileOpenFailed.stage &&
        code.code == piranha::ErrorCode::FileOpenFailed.code;
}

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

es_script::Compiler::Output es_script::Compiler::prepare(const PreparationContext &context,
        std::vector<ScriptInput> &inputs, const InputVerifier &verify) {
    const auto entry = context.entry.u8string();
    if (!context.entry.is_absolute() || !context.libraryRoot.is_absolute() || !context.assetRoot.is_absolute()) {
        throw Error({"paths", entry, "Entry, library root and asset root must be absolute paths"});
    }
    for (const auto &root : {context.libraryRoot, context.assetRoot}) {
        std::error_code error;
        if (!fs::is_directory(root, error)) throw Error({"resources", root.u8string(), "Expected a resource directory"});
    }
    std::vector<ScriptInput> pending;
    Compiler compiler;
    compiler.initialize({context.libraryRoot.u8string(), context.assetRoot.u8string()});
    compiler.m_sourceObserver = [&](const piranha::Path &path, const std::string &bytes) {
        ScriptInput input{fs::canonical(fs::u8path(path.toString())), bytes};
        if (verify) verify(input);
        pending.push_back(std::move(input));
    };
    std::ostringstream diagnostics;
    if (!compiler.compile(entry, diagnostics)) {
        auto diagnostic = compiler.getLastError();
        if (compiler.m_resourceFailure) diagnostic.stage = "resources";
        const auto detail = diagnostics.str();
        if (!detail.empty()) diagnostic.message = detail;
        throw Error(diagnostic);
    }
    auto result = compiler.execute();
    compiler.destroy();
    inputs = std::move(pending);
    return result;
}

void es_script::Compiler::initialize(const std::vector<std::string> &libraryRoots) {
    if (!m_preparation.owns_lock()) m_preparation = std::unique_lock<std::recursive_mutex>(preparationMutex());
    resetCompilation();
    for (const auto &root : libraryRoots) {
        requireUtf8(root);
        if (!fs::u8path(root).is_absolute()) {
            throw Error({"paths", root, "A compiler search root must be an absolute path"});
        }
    }
    m_libraryRoots = libraryRoots;
    if (!m_rules) {
        auto rules = std::make_unique<LanguageRules>();
        rules->initialize();
        m_rules = std::move(rules);
    }
}

bool es_script::Compiler::compile(const std::string &path, std::ostream &diagnostics,
        std::vector<std::string> *resolvedScripts) {
    if (resolvedScripts != nullptr) resolvedScripts->clear();
    resetCompilation();
    m_entryPath = path;
    m_lastError = {};
    m_resourceFailure = false;
    if (!m_rules) {
        m_lastError = {"compile", m_entryPath, "Compiler is not initialized"};
        diagnostics << m_lastError.format() << '\n';
        return false;
    }
    try {
        requireUtf8(path);
        if (!fs::u8path(path).is_absolute()) throw std::invalid_argument("Script entry must be an absolute path");
        m_compiler = new piranha::Compiler(m_rules.get());
        m_compiler->setFileExtension(".mr");
        for (const auto &root : m_libraryRoots) m_compiler->addSearchPath(root);
        m_compiler->setSourceObserver([&](const piranha::Path &source, const std::string &bytes) {
            if (m_sourceObserver) m_sourceObserver(source, bytes);
            requireUtf8(bytes);
        });
        m_program = std::make_unique<piranha::NodeProgram>();
        piranha::IrCompilationUnit *unit = m_compiler->compile(path);
        const auto *errors = m_compiler->getErrorList();
        if (errors->getErrorCount() != 0) {
            m_lastError = {"compile", m_entryPath, "Script contains compilation errors"};
            for (int i = 0; i < errors->getErrorCount(); ++i) {
                const auto *error = errors->getCompilationError(i);
                m_resourceFailure = m_resourceFailure || isFileOpenError(*error);
                printError(error, diagnostics);
            }
            return false;
        }
        if (unit == nullptr) {
            m_resourceFailure = true;
            m_lastError = {"compile", m_entryPath, "Can't find file"};
            diagnostics << "Can't find file: " << m_entryPath << "\n";
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
    catch (const std::bad_alloc &) { throw; }
    catch (const Error &) { throw; }
    catch (const fs::filesystem_error &error) {
        m_resourceFailure = true;
        m_lastError = {"compile", m_entryPath, error.what()};
        diagnostics << m_lastError.format() << '\n';
        return false;
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
    catch (const std::bad_alloc &) { throw; }
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
    m_sourceObserver = {};
    m_rules.reset();
    if (m_preparation.owns_lock()) m_preparation.unlock();
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
