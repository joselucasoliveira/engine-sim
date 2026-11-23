#include "legacy_runtime.hpp"
#include "api_text.hpp"

namespace engine_sim {
struct LayoutFixtureAccess {
    static es_config configuration(const es_runtime &handle) { return handle.implementation.m_config; }
    static void adopt(es_runtime &handle, std::unique_ptr<EngineSession> session, std::uint32_t mode) {
        handle.implementation.adoptSession(std::move(session), {}, mode, false, {});
    }
};
}

namespace engine_sim::legacy_runtime {
std::vector<es_script::ScriptInput> prepare(es_runtime &runtime, const es_script::PreparationContext &paths,
        const std::vector<es_script::ScriptInput> *expected) {
    const auto config = LayoutFixtureAccess::configuration(runtime);
    auto fixture = prepareEngine(paths, {config.simulation_hz, config.mode, config.starter_torque_nm, config.seed}, expected);
    LayoutFixtureAccess::adopt(runtime, std::move(fixture.session), config.mode);
    return std::move(fixture.inputs);
}

es_status load(es_runtime *runtime, const char *entry, const char *library, const char *assets, es_error *error) noexcept {
    if (error) error->message[0] = '\0';
    if (!runtime || !entry || !library || !assets)
        return api::fail(error, ES_INVALID, "Historical comparison requires a runtime and three absolute paths");
    try {
        const es_script::PreparationContext paths{std::filesystem::u8path(entry), std::filesystem::u8path(library),
                                                 std::filesystem::u8path(assets)};
        prepare(*runtime, paths);
        return ES_OK;
    } catch (const std::bad_alloc &) {
        return api::fail(error, ES_NO_MEMORY, "Historical fixture allocation failed");
    } catch (const std::exception &failure) {
        return api::fail(error, ES_MODEL, failure.what());
    } catch (...) {
        return api::fail(error, ES_INTERNAL, "Unexpected historical fixture failure");
    }
}
}
