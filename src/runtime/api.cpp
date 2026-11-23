#include "api_text.hpp"
#include "json_runtime.hpp"
#include "runtime.hpp"
#include "simulation_fault.h"

#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
namespace api = engine_sim::api;
namespace json = engine_sim::json_runtime;
using engine_sim::config_v1::Diagnostic;
using api::fail;

es_status record(std::uint32_t abi, std::uint32_t capacity, std::size_t minimum, es_error *error) noexcept {
    if (abi != ES_ABI_V2) return fail(error, ES_VERSION, "abi: unsupported record version");
    if (capacity < minimum) return fail(error, ES_CAPACITY, "size: record prefix is too small");
    return ES_OK;
}
template<class Record> es_status record(const Record &value, es_error *error) noexcept {
    const auto size = value.size;
    if (size < 2 * sizeof(std::uint32_t))
        return fail(error, ES_CAPACITY, "size: record prefix is too small");
    return record(value.abi, size, sizeof(Record), error);
}
template<class Operation> es_status boundary(es_error *error, Operation operation) noexcept {
    if (error) error->message[0] = 0;
    try { return operation(); }
    catch (const std::bad_alloc &) { return fail(error, ES_NO_MEMORY, "runtime: allocation failed"); }
    catch (const engine_sim::SimulationFault &e) { return fail(error, ES_RENDER_FAULT, e.what()); }
    catch (const std::overflow_error &e) { return fail(error, ES_CAPACITY, e.what()); }
    catch (const std::exception &e) { return fail(error, ES_INTERNAL, e.what()); }
    catch (...) { return fail(error, ES_INTERNAL, "runtime: unexpected library exception"); }
}
bool finiteRange(double value, double maximum) noexcept { return std::isfinite(value) && value >= 0 && value <= maximum; }
es_status configStatus(const es_config &c, es_error *error) noexcept {
    if (c.output_hz == 0 || c.mode > ES_DRIVEN || (c.simulation_hz != 0 && (c.simulation_hz < 4000 || c.simulation_hz > 96000))
        || !finiteRange(c.starter_torque_nm, 10000)) return fail(error, ES_INVALID, "config: invalid rate, mode or starter torque");
    if (c.flags) return fail(error, ES_UNSUPPORTED, "config.flags: unknown flags");
    if (c.output_hz != 44100 && c.output_hz != 48000) return fail(error, ES_UNSUPPORTED, "config.output_hz: rate is unavailable");
    if (c.mode == ES_DRIVEN && c.starter_torque_nm != 0) return fail(error, ES_UNSUPPORTED, "config.starter_torque_nm: unavailable in driven mode");
    return ES_OK;
}
std::uint32_t stage(std::string_view value) noexcept {
    if (value == "parse") return ES_DIAG_PARSE;
    if (value == "shape" || value == "version") return ES_DIAG_SCHEMA;
    if (value == "normalize") return ES_DIAG_NORMALIZE;
    if (value == "graph") return ES_DIAG_SEMANTIC;
    if (value == "resources") return ES_DIAG_RESOURCE;
    if (value == "initialize") return ES_DIAG_PREPARE;
    return ES_DIAG_API;
}

void writeDiagnostic(es_diagnostic &out, const Diagnostic &diagnostic) noexcept {
    es_diagnostic value{};
    value.size = out.size;
    value.abi = out.abi;
    value.stage = diagnostic.code.empty() ? ES_DIAG_NONE : stage(diagnostic.stage);
    value.byte_offset = diagnostic.byte_offset.value_or(ES_DIAG_NO_LOCATION);
    value.line = diagnostic.line.value_or(ES_DIAG_NO_LOCATION);
    value.column = diagnostic.column.value_or(ES_DIAG_NO_LOCATION);
    api::copy(value.code, diagnostic.code);
    if (api::copy(value.pointer, diagnostic.pointer)) value.truncated |= ES_DIAG_TRUNC_POINTER;
    if (diagnostic.component_id && api::copy(value.component_id, *diagnostic.component_id))
        value.truncated |= ES_DIAG_TRUNC_COMPONENT;
    if (diagnostic.field && api::copy(value.field, *diagnostic.field)) value.truncated |= ES_DIAG_TRUNC_FIELD;
    if (diagnostic.path && api::copy(value.path, *diagnostic.path)) value.truncated |= ES_DIAG_TRUNC_PATH;
    if (api::copy(value.message, diagnostic.message)) value.truncated |= ES_DIAG_TRUNC_MESSAGE;
    std::memcpy(&out, &value, sizeof(value));
}

es_status diagnosticFailure(es_diagnostic *detail, es_error *error, es_status status,
                             std::uint32_t stage, std::string_view code, std::string_view message) noexcept {
    if (detail) {
        writeDiagnostic(*detail, {});
        detail->stage = stage;
        api::copy(detail->code, code);
        api::copy(detail->message, message);
    }
    return fail(error, status, message);
}

bool path(const char *text, std::filesystem::path &value) {
    if (!text || !api::utf8(text)) return false;
    value = std::filesystem::u8path(text);
    return value.is_absolute();
}

template<class Operation>
es_status jsonBoundary(es_diagnostic *detail, es_error *error, const Operation &operation) noexcept {
    if (error) error->message[0] = '\0';
    if (detail) {
        const auto status = record(*detail, error);
        if (status != ES_OK) return status;
    }
    Diagnostic failure;
    es_status status;
    try {
        status = operation(detail ? &failure : nullptr);
    } catch (const std::bad_alloc &) {
        status = ES_NO_MEMORY;
    } catch (...) {
        status = ES_INTERNAL;
    }
    if (status == ES_NO_MEMORY)
        return diagnosticFailure(detail, error, status, ES_DIAG_PREPARE, "CFG_MEMORY", "Allocation failed");
    if (status == ES_INTERNAL)
        return diagnosticFailure(detail, error, status, ES_DIAG_API, "API_INTERNAL", "Unexpected runtime exception");
    if (status == ES_RESOURCE && detail && failure.code.empty())
        return diagnosticFailure(detail, error, status, ES_DIAG_RESOURCE, "CFG_RESOURCE_PATH", "Filesystem operation failed");
    if (detail) writeDiagnostic(*detail, failure);
    return status;
}

es_status invalid(Diagnostic *detail, es_error *error, const char *message) {
    if (detail) {
        detail->code = "API_INVALID";
        detail->stage = "api";
        detail->message = message;
    }
    return fail(error, ES_INVALID, message);
}
}

extern "C" {
uint32_t ES_CALL es_api_version(void) { return ES_ABI_V2; }
uint32_t ES_CALL es_capabilities(void) { return ES_CAP_FREE | ES_CAP_44100 | ES_CAP_48000 | ES_CAP_LOAD | ES_CAP_DRIVEN | ES_CAP_RESET | ES_CAP_PARALLEL_RENDER
    | ES_CAP_JSON_FILE | ES_CAP_JSON_BYTES | ES_CAP_CONFIG_DIAGNOSTIC; }
es_status ES_CALL es_config_init(uint32_t abi, uint32_t capacity, es_config *out, es_error *error) {
    return boundary(error, [&] {
        if (!out) return fail(error, ES_INVALID, "config: NULL destination");
        const auto status = record(abi, capacity, sizeof(es_config), error); if (status) return status;
        const es_config defaults{capacity, abi, 44100, ES_FREE, 1, 0, 0, 0};
        std::memcpy(out, &defaults, sizeof(defaults)); return ES_OK;
    });
}
es_status ES_CALL es_create(const es_config *config, es_runtime **out, es_error *error) {
    if (out) *out = nullptr;
    return boundary(error, [&] {
        if (!config || !out) return fail(error, ES_INVALID, "create: config and output slot are required");
        auto status = record(*config, error); if (status) return status;
        status = configStatus(*config, error); if (status) return status;
        *out = new es_runtime(*config); return ES_OK;
    });
}
es_status ES_CALL es_load_json_file(es_runtime *rt, const char *entry, const char *root,
                                    es_diagnostic *detail, es_error *error) {
    return jsonBoundary(detail, error, [&](Diagnostic *problem) {
        std::filesystem::path entryPath, rootPath;
        if (!rt || !path(entry, entryPath) || !path(root, rootPath))
            return invalid(problem, error, "A runtime and absolute UTF-8 entry and asset paths are required");
        return json::load_file(rt, entryPath, rootPath, problem, error);
    });
}
es_status ES_CALL es_load_json_bytes(es_runtime *rt, const uint8_t *bytes, uint64_t length, const char *root,
                                     es_diagnostic *detail, es_error *error) {
    return jsonBoundary(detail, error, [&](Diagnostic *problem) {
        std::filesystem::path rootPath;
        if (!rt || !path(root, rootPath))
            return invalid(problem, error, "A runtime and absolute UTF-8 asset path are required");
        return json::load_bytes(rt, reinterpret_cast<const char *>(bytes), length, rootPath, problem, error);
    });
}
es_status ES_CALL es_set_controls(es_runtime *rt, const es_controls *controls, es_error *error) {
    return boundary(error, [&] {
        if (!rt || !controls) return fail(error, ES_INVALID, "controls: handle and vector are required");
        auto status = record(*controls, error); if (status) return status;
        if (controls->ignition > 1 || controls->starter > 1 || !finiteRange(controls->demand, 1)
            || !finiteRange(controls->load_nm, 10000) || !finiteRange(controls->driven_rpm, 12000))
            return fail(error, ES_INVALID, "controls: invalid boolean or numeric range");
        const auto &t = rt->implementation.telemetry();
        if ((t.mode == ES_FREE && controls->driven_rpm != 0) || (t.mode == ES_DRIVEN && controls->starter != 0))
            return fail(error, ES_UNSUPPORTED, "controls: incompatible starter or driven speed for mode");
        if (t.mode == ES_DRIVEN && t.state == ES_READY && controls->driven_rpm > t.driven_limit_rpm)
            return fail(error, ES_INVALID, "controls.driven_rpm: exceeds loaded engine limit");
        if (rt->implementation.telemetry().state != ES_READY) return fail(error, ES_STATE, "controls: ready engine required");
        rt->implementation.setControls(*controls); return ES_OK;
    });
}
es_status ES_CALL es_render(es_runtime *rt, uint32_t frames, float *mono, uint32_t capacity, uint32_t *written, es_error *error) {
    if (written) *written = 0;
    return boundary(error, [&] {
        if (!rt || !written || (frames && !mono)) return fail(error, ES_INVALID, "render: handle, count destination and nonempty PCM span are required");
        if (frames > ES_MAX_FRAMES || frames > capacity || capacity > std::numeric_limits<std::size_t>::max() / sizeof(float))
            return fail(error, ES_CAPACITY, "render: output capacity exceeded");
        if (rt->implementation.telemetry().state != ES_READY) return fail(error, ES_STATE, "render: ready engine required");
        rt->implementation.render(mono, frames, *written); return ES_OK;
    });
}
es_status ES_CALL es_get_telemetry(const es_runtime *rt, es_telemetry *out, es_error *error) {
    return boundary(error, [&] {
        if (!rt || !out) return fail(error, ES_INVALID, "telemetry: handle and destination are required");
        const auto status = record(*out, error); if (status) return status;
        auto value = rt->implementation.telemetry(); value.size = out->size; value.abi = out->abi;
        std::memcpy(out, &value, sizeof(value)); return ES_OK;
    });
}
es_status ES_CALL es_reset(es_runtime *rt, es_error *error) {
    return es_reset_detailed(rt, nullptr, error);
}
es_status ES_CALL es_reset_detailed(es_runtime *rt, es_diagnostic *detail, es_error *error) {
    return jsonBoundary(detail, error, [&](Diagnostic *problem) { return json::reset(rt, problem, error); });
}
es_status ES_CALL es_unload(es_runtime *rt, es_error *error) {
    return boundary(error, [&] {
        if (!rt) return fail(error, ES_INVALID, "unload: handle is required");
        rt->implementation.unload(); return ES_OK;
    });
}
void ES_CALL es_destroy(es_runtime *rt) { delete rt; }
}
