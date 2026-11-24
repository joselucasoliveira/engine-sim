#include "json_runtime.hpp"
#include "api_text.hpp"
#include "json_session.hpp"
#include "runtime.hpp"
#include "preparation.hpp"
#include "read.hpp"
#include "shaft_drive.hpp"
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

    static std::size_t fleet_session_bytes(bool driven) {
        return sizeof(Runtime::Session) + sizeof(PullEngine) + (driven ? sizeof(DrivenShaft) : 0);
    }

    static const JsonRecipe *saved(const Runtime &runtime) {
        if (!runtime.m_session) return nullptr;
        return runtime.m_session->prepared.recipe.get();
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

Outcome read_document(JsonRecipe &recipe, const JsonRecipe *expected, bool fleet = false) {
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
    if (fleet && static_cast<std::uint64_t>(size) > assembly::fleet_document_limit) {
        auto problem = assembly::extent_error("document bytes", static_cast<std::uint64_t>(size), assembly::fleet_document_limit);
        problem.stage = "parse";
        problem.path = recipe.resolved_entry.u8string();
        return problem;
    }
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
    constexpr cfg::json_detail::ModelLimits fleet_limits{
        assembly::fleet_document_limit, assembly::fleet_definition_limit,
        assembly::fleet_curve_limit, assembly::fleet_point_limit};
    auto parsed = profile.fleet ? cfg::json_detail::read_limited_model(recipe.bytes, fleet_limits)
                                : config_json::read_model(recipe.bytes);
    if (auto *problem = std::get_if<cfg::Diagnostic>(&parsed)) {
        if (!recipe.resolved_entry.empty()) problem->path = recipe.resolved_entry.u8string();
        return std::move(*problem);
    }
    if (checkpoint) checkpoint(Phase::normalized);
    const auto &model = std::get<cfg::NormalizedDocument>(parsed);
    if (profile.fleet) {
        auto &report = profile.fleet->capacity;
        report.document_bytes = recipe.bytes.size();
        report.recipe_bytes = sizeof(JsonRecipe) + recipe.bytes.capacity() + 1 +
            assembly::path_capacity(recipe.entry) + assembly::path_capacity(recipe.resolved_entry) +
            assembly::path_capacity(recipe.root);
        report.graph_bytes = recipe.resolved_entry.u8string().size() + 1;
        report.session_bytes = JsonRuntimeAccess::fleet_session_bytes(profile.mode == assembly::Mode::driven) +
            sizeof(double) * (4 * model.topology().exhausts.size() + model.topology().intakes.size()) +
            2 * 64 * sizeof(float);
    }
    auto result = assembly::prepare_session(model, recipe.root, profile,
                                            recipe.resolved_entry.u8string(), expected ? &expected->resources : nullptr);
    if (auto *problem = std::get_if<cfg::Diagnostic>(&result)) return std::move(*problem);
    if (checkpoint) checkpoint(Phase::prepared);
    auto prepared = std::get<assembly::PreparedSession>(std::move(result));
    recipe.resources = std::move(prepared.inputs);
    bool governor = false;
    if (profile.fleet) {
        const auto &values = model.values();
        const auto &throttles = values.components.throttles;
        governor = std::any_of(throttles.begin(), throttles.end(), [&](const cfg::Throttle &value) {
            const auto *candidate = std::get_if<cfg::Governor>(&value);
            return candidate && candidate->id == values.engine.throttle;
        });
    }
    return PreparedModel{std::move(prepared.session), std::make_shared<const JsonRecipe>(std::move(recipe)), governor};
}

PreparedModelResult prepare_fleet(JsonRecipe recipe, const es_config &config, assembly::FleetCapacity &capacity,
                                  const Checkpoint &checkpoint) {
    if (config.mode != ES_FREE && config.mode != ES_DRIVEN) {
        auto problem = issue("API_INVALID", "api", "Fleet mode must be FREE or DRIVEN");
        problem.field = "mode";
        return problem;
    }
    assembly::FleetPreparation fleet{config.output_hz};
    const auto mode = config.mode == ES_DRIVEN ? assembly::Mode::driven : assembly::Mode::free;
    auto result = prepare(std::move(recipe), nullptr,
                          {mode, config.simulation_hz, config.starter_torque_nm, config.seed, &fleet}, checkpoint);
    if (std::holds_alternative<PreparedModel>(result)) capacity = fleet.capacity;
    return result;
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

PreparedModelResult prepare_fleet_file(const fs::path &entry, const fs::path &root, const es_config &config,
                                       assembly::FleetCapacity &capacity, const Checkpoint &checkpoint) {
    if (!absolute_path(root)) return issue("API_INVALID", "api", "An absolute resource root is required", root);
    JsonRecipe recipe;
    recipe.entry = entry;
    recipe.root = root;
    if (auto problem = read_document(recipe, nullptr, true)) return std::move(*problem);
    return prepare_fleet(std::move(recipe), config, capacity, checkpoint);
}

PreparedModelResult prepare_fleet_bytes(const char *bytes, std::uint64_t size, const fs::path &root,
                                        const es_config &config, assembly::FleetCapacity &capacity,
                                        const Checkpoint &checkpoint) {
    if (!absolute_path(root) || (!bytes && size))
        return issue("API_INVALID", "api", "A valid byte span and absolute resource root are required");
    if (size > assembly::fleet_document_limit) {
        auto problem = assembly::extent_error("document bytes", size, assembly::fleet_document_limit);
        problem.stage = "parse";
        return problem;
    }
    JsonRecipe recipe;
    recipe.root = root;
    if (size) recipe.bytes.assign(bytes, static_cast<std::size_t>(size));
    return prepare_fleet(std::move(recipe), config, capacity, checkpoint);
}

PreparedModelResult reset_desktop(const JsonRecipe &saved, std::uint64_t seed) {
    JsonRecipe recipe;
    recipe.entry = saved.entry;
    recipe.root = saved.root;
    if (auto problem = read_document(recipe, &saved)) return std::move(*problem);
    return prepare(std::move(recipe), &saved, {assembly::Mode::desktop, 0, 0, seed}, {});
}

PreparedModel require_model(PreparedModelResult result, const fs::path &entry) {
    if (const auto *error = std::get_if<cfg::Diagnostic>(&result)) {
        auto location = error->code;
        if (!error->pointer.empty()) location += " " + error->pointer;
        if (error->line && error->column)
            location += " line " + std::to_string(*error->line) + ", column " + std::to_string(*error->column);
        throw EngineLoadError({error->stage, error->path.value_or(entry.u8string()), location + "\n" + error->message});
    }
    return std::get<PreparedModel>(std::move(result));
}

void visit_file_inputs(const JsonRecipe &recipe, const InputVisitor &visitor) {
    if (!recipe.resolved_entry.empty()) visitor(recipe.resolved_entry, recipe.bytes);
    for (const auto &resource : recipe.resources.files) {
        visitor(resource.path, {reinterpret_cast<const char *>(resource.bytes.data()), resource.bytes.size()});
    }
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
