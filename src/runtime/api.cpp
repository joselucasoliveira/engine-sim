#include "runtime.hpp"
#include "simulation_fault.h"
#include "constants.h"
#include "piston_engine_simulator.h"
#include "shaft_drive.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
// Validate scalar UTF-8 without allocating, including overlongs and surrogates.
std::size_t character(const unsigned char *s) noexcept {
    const auto c = s[0];
    if (c < 0x80) return c ? 1 : 0;
    const unsigned length = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    if (!length) return 0;
    for (unsigned i = 1; i < length; ++i) if (s[i] < 0x80 || s[i] > 0xbf) return 0;
    if ((c == 0xe0 && s[1] < 0xa0) || (c == 0xed && s[1] > 0x9f)
        || (c == 0xf0 && s[1] < 0x90) || (c == 0xf4 && s[1] > 0x8f)) return 0;
    return length;
}
bool utf8(const char *text) noexcept {
    const auto *p = reinterpret_cast<const unsigned char *>(text);
    while (*p) { const auto count = character(p); if (!count) return false; p += count; }
    return true;
}
es_status fail(es_error *error, es_status status, const char *text) noexcept {
    if (error) {
        std::size_t at = 0;
        auto *input = reinterpret_cast<const unsigned char *>(text);
        while (*input && at < sizeof(error->message) - 1) {
            const auto count = character(input);
            if (!count) { error->message[at++] = '?'; ++input; continue; }
            if (count > sizeof(error->message) - 1 - at) break;
            std::memcpy(error->message + at, input, count); at += count; input += count;
        }
        error->message[at] = 0;
    }
    return status;
}
es_status record(std::uint32_t abi, std::uint32_t capacity, std::size_t minimum, es_error *error) noexcept {
    if ((abi >> 16) != 1 || (abi & 0xffff) != 0) return fail(error, ES_VERSION, "abi: unsupported record version");
    if (capacity < minimum) return fail(error, ES_CAPACITY, "size: record prefix is too small");
    return ES_OK;
}
template<class Operation> es_status boundary(es_error *error, Operation operation) noexcept {
    if (error) error->message[0] = 0;
    try { return operation(); }
    catch (const std::bad_alloc &) { return fail(error, ES_NO_MEMORY, "runtime: allocation failed"); }
    catch (const EngineLoadError &e) {
        const auto &stage = e.diagnostic.stage;
        const auto status = stage == "resources" ? ES_RESOURCE
            : stage == "compile" || stage == "execute" ? ES_SCRIPT
            : stage == "unsupported" ? ES_UNSUPPORTED
            : stage == "paths" || stage == "configuration" ? ES_INVALID : ES_MODEL;
        return fail(error, status, e.what());
    }
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
es_status pathStatus(const char *text, bool directory, std::filesystem::path &out, es_error *error) {
    if (!utf8(text)) return fail(error, ES_INVALID, "paths: malformed UTF-8");
    out = std::filesystem::u8path(text);
    if (!out.is_absolute()) return fail(error, ES_INVALID, "paths: absolute entry, library and asset paths are required");
    std::error_code failure;
    const bool exists = directory ? std::filesystem::is_directory(out, failure) : std::filesystem::is_regular_file(out, failure);
    if (!exists || failure) {
        const auto message = std::string("resources: ") + text + (directory ? ": expected an existing directory" : ": expected an existing entry file");
        return fail(error, ES_RESOURCE, message.c_str());
    }
    return ES_OK;
}
}

extern "C" {
uint32_t ES_CALL es_api_version(void) { return ES_ABI_V1; }
uint32_t ES_CALL es_capabilities(void) { return ES_CAP_FREE | ES_CAP_44100 | ES_CAP_48000 | ES_CAP_LOAD | ES_CAP_DRIVEN | ES_CAP_RESET | ES_CAP_PARALLEL_RENDER; }
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
        auto status = record(config->abi, config->size, sizeof(es_config), error); if (status) return status;
        status = configStatus(*config, error); if (status) return status;
        *out = new es_runtime(*config); return ES_OK;
    });
}
es_status ES_CALL es_load(es_runtime *rt, const char *entry, const char *library, const char *assets, es_error *error) {
    return boundary(error, [&] {
        if (!rt || !entry || !library || !assets) return fail(error, ES_INVALID, "load: handle and all paths are required");
        es_script::PreparationContext recipe;
        for (const auto item : {std::make_pair(entry, &recipe.entry), std::make_pair(library, &recipe.libraryRoot), std::make_pair(assets, &recipe.assetRoot)}) {
            const auto status = pathStatus(item.first, item.second != &recipe.entry, *item.second, error); if (status) return status;
        }
        rt->implementation.load(recipe); return ES_OK;
    });
}
es_status ES_CALL es_set_controls(es_runtime *rt, const es_controls *controls, es_error *error) {
    return boundary(error, [&] {
        if (!rt || !controls) return fail(error, ES_INVALID, "controls: handle and vector are required");
        auto status = record(controls->abi, controls->size, sizeof(es_controls), error); if (status) return status;
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
        const auto status = record(out->abi, out->size, sizeof(es_telemetry), error); if (status) return status;
        auto value = rt->implementation.telemetry(); value.size = out->size; value.abi = out->abi;
        std::memcpy(out, &value, sizeof(value)); return ES_OK;
    });
}
es_status ES_CALL es_reset(es_runtime *rt, es_error *error) {
    return boundary(error, [&] {
        if (!rt) return fail(error, ES_INVALID, "reset: handle is required");
        if (rt->implementation.telemetry().state == ES_EMPTY) return fail(error, ES_STATE, "reset: prepared engine required");
        rt->implementation.reset(); return ES_OK;
    });
}
es_status ES_CALL es_unload(es_runtime *rt, es_error *error) {
    return boundary(error, [&] {
        if (!rt) return fail(error, ES_INVALID, "unload: handle is required");
        rt->implementation.unload(); return ES_OK;
    });
}
void ES_CALL es_destroy(es_runtime *rt) { delete rt; }
}
