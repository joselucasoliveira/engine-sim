#include "legacy_runtime.hpp"
#include "api_text.hpp"

#include <new>
#include <stdexcept>

namespace engine_sim::legacy_runtime {
void Access::load(Runtime &runtime, const es_script::PreparationContext &paths) {
    if (runtime.m_profile != SampleClock::Profile::Public)
        throw std::logic_error("Desktop load requires staged bindings");
    const auto &config = runtime.m_config;
    adopt(runtime, prepareEngine(paths, {config.simulation_hz, config.mode, config.starter_torque_nm, config.seed}));
}

void Access::reset(Runtime &runtime) {
    const auto *recipe = saved(runtime);
    if (!recipe || runtime.m_profile != SampleClock::Profile::Public)
        throw std::logic_error("Reset requires a prepared legacy fixture");
    adopt(runtime, prepareEngine(recipe->paths, recipe->config, &recipe->inputs));
}

void Access::adopt(Runtime &runtime, PreparedEngine engine, PullEngine::ControlLatch controls) {
    auto recipe = std::make_shared<const Recipe>(Recipe{std::move(engine.recipe), engine.config, std::move(engine.inputs)});
    runtime.adoptSession(std::move(engine.session), std::move(recipe), engine.config.mode,
                         engine.config.desktop, std::move(controls));
}

const Recipe *Access::saved(const Runtime &runtime) {
    if (!runtime.m_session) return nullptr;
    const auto *slot = std::get_if<std::shared_ptr<const Recipe>>(&runtime.m_session->prepared.recipe);
    return slot ? slot->get() : nullptr;
}

namespace {
namespace fs = std::filesystem;
using api::fail;

es_status pathStatus(const char *input, bool directory, fs::path &out, es_error *error) {
    if (!api::utf8(input)) return fail(error, ES_INVALID, "paths: malformed UTF-8");
    out = fs::u8path(input);
    if (!out.is_absolute()) return fail(error, ES_INVALID, "paths: absolute entry, library and asset paths are required");
    std::error_code failure;
    const bool exists = directory ? fs::is_directory(out, failure) : fs::is_regular_file(out, failure);
    if (exists && !failure) return ES_OK;
    return fail(error, ES_RESOURCE, std::string("resources: ") + input +
        (directory ? ": expected an existing directory" : ": expected an existing entry file"));
}

template<class Operation> es_status boundary(es_error *error, const Operation &operation) noexcept {
    if (error) error->message[0] = '\0';
    try { return operation(); }
    catch (const std::bad_alloc &) { return fail(error, ES_NO_MEMORY, "runtime: allocation failed"); }
    catch (const EngineLoadError &failure) {
        const auto &stage = failure.diagnostic.stage;
        const auto status = stage == "resources" ? ES_RESOURCE
            : stage == "compile" || stage == "execute" ? script_error
            : stage == "unsupported" ? ES_UNSUPPORTED
            : stage == "paths" || stage == "configuration" ? ES_INVALID : ES_MODEL;
        return fail(error, status, failure.what());
    }
    catch (const std::exception &failure) { return fail(error, ES_INTERNAL, failure.what()); }
    catch (...) { return fail(error, ES_INTERNAL, "runtime: unexpected library exception"); }
}
}

es_status load(es_runtime *runtime, const char *entry, const char *library, const char *assets, es_error *error) noexcept {
    return boundary(error, [&] {
        if (!runtime || !entry || !library || !assets)
            return fail(error, ES_INVALID, "load: handle and all paths are required");
        es_script::PreparationContext paths;
        for (const auto item : {std::make_pair(entry, &paths.entry), std::make_pair(library, &paths.libraryRoot),
                                std::make_pair(assets, &paths.assetRoot)}) {
            const auto status = pathStatus(item.first, item.second != &paths.entry, *item.second, error);
            if (status != ES_OK) return status;
        }
        Access::load(runtime->implementation, paths);
        return ES_OK;
    });
}

es_status reset(es_runtime *runtime, es_error *error) noexcept {
    return boundary(error, [&] {
        if (!runtime) return fail(error, ES_INVALID, "reset: handle is required");
        if (!Access::saved(runtime->implementation)) return fail(error, ES_STATE, "reset: prepared engine required");
        Access::reset(runtime->implementation);
        return ES_OK;
    });
}
}
