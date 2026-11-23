#include "json_runtime.hpp"
#include "api_text.hpp"
#include "json_session.hpp"
#include "runtime.hpp"
#include "preparation.hpp"
#include "simulation_fault.h"

#include <engine_sim/config-json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <optional>
#include <string_view>
#include <utility>

namespace engine_sim {
namespace fs = std::filesystem;
namespace cfg = config_v1;

struct JsonRecipe {
    // Byte-backed loads leave both entry paths empty.
    fs::path entry;
    fs::path resolved_entry;
    fs::path root;
    std::string bytes;
    assembly::ResourceInputs resources;
};

struct JsonRuntimeAccess {
    static es_config config(const Runtime &runtime) { return runtime.m_config; }

    static const JsonRecipe *saved(const Runtime &runtime) {
        if (!runtime.m_session) return nullptr;
        const auto *slot = std::get_if<std::shared_ptr<const JsonRecipe>>(&runtime.m_session->prepared.recipe);
        return slot ? slot->get() : nullptr;
    }

    static void adopt(Runtime &runtime, json_runtime::PreparedModel prepared, const json_runtime::Checkpoint &checkpoint) {
        runtime.adoptSession(std::move(prepared.session), std::move(prepared.recipe), runtime.m_config.mode, false, {},
                             [&] { if (checkpoint) checkpoint(json_runtime::Phase::snapshot); });
    }
};

namespace json_runtime {
namespace {
constexpr std::uint64_t document_limit = 16 * 1024 * 1024;

bool absolute_path(const fs::path &path) {
    return path.is_absolute() && path.native().find(fs::path::value_type{}) == fs::path::string_type::npos;
}

cfg::Diagnostic issue(std::string_view code, std::string_view stage, std::string_view message,
                      const fs::path &path = {}) {
    cfg::Diagnostic result;
    result.code = code;
    result.stage = stage;
    result.message = message;
    if (!path.empty()) result.path = path.u8string();
    return result;
}

es_status result_status(const cfg::Diagnostic &problem) {
    if (problem.code == "API_INVALID") return ES_INVALID;
    if (problem.code == "API_CAPACITY") return ES_CAPACITY;
    if (problem.code == "API_STATE") return ES_STATE;
    if (problem.code == "CFG_VERSION") return ES_VERSION;
    if (problem.stage == "resources") return ES_RESOURCE;
    if (problem.stage == "graph" || problem.stage == "initialize") return ES_MODEL;
    return ES_CONFIG;
}

using Outcome = std::optional<cfg::Diagnostic>;

template<class Operation>
es_status boundary(cfg::Diagnostic *detail, es_error *error, const Operation &run) noexcept {
    if (detail) *detail = {};
    api::fail(error, ES_OK, {});
    try {
        auto failure = run();
        if (!failure) return ES_OK;
        const auto status = result_status(*failure);
        if (error) api::fail(error, status, failure->message);
        if (detail) *detail = std::move(*failure);
        return status;
    } catch (const std::bad_alloc &) {
        return api::fail(error, ES_NO_MEMORY, "CFG_MEMORY: allocation failed");
    } catch (const fs::filesystem_error &) {
        return api::fail(error, ES_RESOURCE, "CFG_RESOURCE_PATH: filesystem operation failed");
    } catch (...) {
        return api::fail(error, ES_INTERNAL, "API_INTERNAL: unexpected runtime exception");
    }
}

Outcome read_document(JsonRecipe &recipe, const JsonRecipe *expected) {
    if (!absolute_path(recipe.entry))
        return issue("API_INVALID", "api", "An absolute JSON entry path is required", recipe.entry);
    std::error_code error;
    recipe.resolved_entry = fs::canonical(recipe.entry, error);
    if (error || !fs::is_regular_file(recipe.resolved_entry, error) || error)
        return issue("CFG_RESOURCE_PATH", "resources", "JSON entry must resolve to a regular file", recipe.entry);
    if (expected && recipe.resolved_entry != expected->resolved_entry)
        return issue("CFG_RESOURCE_BYTES", "resources", "Resolved JSON entry changed", recipe.entry);

    std::ifstream stream(recipe.resolved_entry, std::ios::binary | std::ios::ate);
    const auto size = stream.tellg();
    if (size < 0)
        return issue("CFG_RESOURCE_BYTES", "resources", "Cannot open JSON entry", recipe.resolved_entry);
    if (expected && static_cast<std::uint64_t>(size) != expected->bytes.size())
        return issue("CFG_RESOURCE_BYTES", "resources", "JSON entry extent changed", recipe.resolved_entry);
    if (static_cast<std::uint64_t>(size) > document_limit)
        return issue("CFG_LIMIT", "parse", "JSON entry exceeds the 16 MiB document limit", recipe.resolved_entry);
    recipe.bytes.resize(static_cast<std::size_t>(size));
    stream.seekg(0);
    if ((!recipe.bytes.empty() && !stream.read(recipe.bytes.data(), static_cast<std::streamsize>(size))) ||
        !stream || stream.peek() != std::char_traits<char>::eof() || stream.bad())
        return issue("CFG_RESOURCE_BYTES", "resources", "Cannot read exactly one complete JSON entry", recipe.resolved_entry);
    if (expected && recipe.bytes != expected->bytes)
        return issue("CFG_RESOURCE_BYTES", "resources", "JSON entry bytes changed", recipe.resolved_entry);
    return {};
}

PreparedModelResult prepare(JsonRecipe recipe, const JsonRecipe *expected,
                            assembly::PreparationProfile profile, const Checkpoint &checkpoint) {
    if (checkpoint) checkpoint(Phase::document);
    auto parsed = cfg::read_json(recipe.bytes);
    if (auto *problem = std::get_if<cfg::Diagnostic>(&parsed)) {
        if (!recipe.resolved_entry.empty()) problem->path = recipe.resolved_entry.u8string();
        return std::move(*problem);
    }
    if (checkpoint) checkpoint(Phase::normalized);
    auto result = assembly::prepare_session(std::get<cfg::NormalizedDocument>(parsed), recipe.root, profile,
                                            recipe.resolved_entry.u8string(), expected ? &expected->resources : nullptr);
    if (auto *problem = std::get_if<cfg::Diagnostic>(&result)) return std::move(*problem);
    if (checkpoint) checkpoint(Phase::prepared);
    auto prepared = std::get<assembly::PreparedSession>(std::move(result));
    recipe.resources = std::move(prepared.inputs);
    return PreparedModel{std::move(prepared.session), std::make_shared<const JsonRecipe>(std::move(recipe))};
}

Outcome activate(Runtime &runtime, JsonRecipe recipe, const JsonRecipe *expected, const Checkpoint &checkpoint) {
    const auto config = JsonRuntimeAccess::config(runtime);
    const auto mode = config.mode == ES_DRIVEN ? assembly::Mode::driven : assembly::Mode::free;
    auto prepared = prepare(std::move(recipe), expected, {mode, config.simulation_hz, config.starter_torque_nm, config.seed},
                            checkpoint);
    if (auto *problem = std::get_if<cfg::Diagnostic>(&prepared)) return std::move(*problem);
    try {
        JsonRuntimeAccess::adopt(runtime, std::get<PreparedModel>(std::move(prepared)), checkpoint);
    } catch (const SimulationFault &) {
        return issue("CFG_INITIALIZE", "initialize", "Initial session snapshot could not be constructed");
    }
    return {};
}
}

PreparedModelResult prepare_desktop(const fs::path &entry, const fs::path &root, std::uint64_t seed) {
    if (!absolute_path(root))
        return issue("API_INVALID", "api", "An absolute resource root is required", root);
    JsonRecipe recipe;
    recipe.entry = entry;
    recipe.root = root;
    if (auto problem = read_document(recipe, nullptr)) return std::move(*problem);
    return prepare(std::move(recipe), nullptr, {assembly::Mode::desktop, 0, 0, seed}, {});
}

PreparedModelResult reset_desktop(const JsonRecipe &saved, std::uint64_t seed) {
    JsonRecipe recipe;
    recipe.entry = saved.entry;
    recipe.root = saved.root;
    if (auto problem = read_document(recipe, &saved)) return std::move(*problem);
    return prepare(std::move(recipe), &saved, {assembly::Mode::desktop, 0, 0, seed}, {});
}

es_status load_file(es_runtime *runtime, const fs::path &entry, const fs::path &root,
                    cfg::Diagnostic *detail, es_error *error, const Checkpoint &checkpoint) noexcept {
    return boundary(detail, error, [&]() -> Outcome {
        if (!runtime || !absolute_path(root))
            return issue("API_INVALID", "api", "A runtime and absolute resource root are required");
        JsonRecipe recipe;
        recipe.entry = entry;
        recipe.root = root;
        if (auto problem = read_document(recipe, nullptr)) return problem;
        return activate(runtime->implementation, std::move(recipe), nullptr, checkpoint);
    });
}

es_status load_bytes(es_runtime *runtime, const char *bytes, std::uint64_t size, const fs::path &root,
                     cfg::Diagnostic *detail, es_error *error, const Checkpoint &checkpoint) noexcept {
    return boundary(detail, error, [&]() -> Outcome {
        if (!runtime || !absolute_path(root) || (!bytes && size != 0))
            return issue("API_INVALID", "api", "A runtime, valid byte span and absolute resource root are required");
        if (size > std::numeric_limits<std::size_t>::max() || size > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()))
            return issue("API_CAPACITY", "api", "Byte extent exceeds host addressability");
        if (size > document_limit)
            return issue("CFG_LIMIT", "parse", "JSON document exceeds the 16 MiB document limit");
        JsonRecipe recipe;
        recipe.root = root;
        if (size != 0) recipe.bytes.assign(bytes, static_cast<std::size_t>(size));
        return activate(runtime->implementation, std::move(recipe), nullptr, checkpoint);
    });
}

es_status reset(es_runtime *runtime, cfg::Diagnostic *detail, es_error *error, const Checkpoint &checkpoint) noexcept {
    return boundary(detail, error, [&]() -> Outcome {
        if (!runtime) return issue("API_INVALID", "api", "A runtime is required");
        const auto *saved = JsonRuntimeAccess::saved(runtime->implementation);
        if (!saved) return issue("API_STATE", "api", "There is no active JSON recipe");
        JsonRecipe next;
        next.entry = saved->entry;
        next.root = saved->root;
        if (next.entry.empty()) next.bytes = saved->bytes;
        else if (auto problem = read_document(next, saved)) return problem;
        return activate(runtime->implementation, std::move(next), saved, checkpoint);
    });
}

}
}
