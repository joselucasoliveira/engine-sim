#include "owner.hpp"
#include "api_text.hpp"

#include <cmath>
#include <filesystem>
#include <limits>
#include <string_view>

namespace engine_sim::fleet {
Voice::Voice(const es_config &config, const es_rt_voice_config &settings,
             assembly::FleetCapacity estimate, json_runtime::PreparedModel graph)
    : runtime(config), capacity(estimate), initial(settings.initial), controlsPolicy(settings.controls_policy),
      mode(settings.mode), requested(settings.initial) {
    runtime.adoptSession(std::move(graph.session), std::move(graph.recipe), config.mode, false, {});
    const auto *system = runtime.m_session->prepared.session->simulator->getSystem();
    auto &actual = capacity.assembled;
    actual.bodies = static_cast<std::size_t>(system->getRigidBodyCount());
    actual.constraints = static_cast<std::size_t>(system->getConstraintCount());
    actual.rows = static_cast<std::size_t>(system->getFullConstraintCount());
    actual.generators = static_cast<std::size_t>(system->getForceGeneratorCount());
    const auto &expected = capacity.predicted;
    if (actual.bodies != expected.bodies || actual.constraints != expected.constraints ||
        actual.rows != expected.rows || actual.generators != expected.generators)
        throw SimulationFault("Final voice graph differs from its admitted solver dimensions");
    drivenLimitRpm = runtime.telemetry().driven_limit_rpm;
    if (controlsPolicy == ES_RT_GAME) {
        game.emplace(initial, runtime.telemetry().simulation_hz, graph.governor);
        runtime.m_session->shapeContext = this;
        runtime.m_session->shapeControls = [](void *source) noexcept {
            auto &voice = *static_cast<Voice *>(source);
            const auto tick = voice.nextTick;
            if (tick != UINT64_MAX) ++voice.nextTick;
            const auto applied = voice.game->at(tick, voice.requested);
            voice.applied = applied;
            return es_controls{sizeof(es_controls), ES_ABI_V2, applied.ignition, applied.starter,
                               applied.demand, applied.load_nm, applied.driven_rpm};
        };
    }
}

es_rt_lane_info Voice::laneInfo(es_rt_id id, std::uint32_t state) const noexcept {
    const auto &raw = runtime.telemetry();
    es_rt_lane_info info{};
    info.voice = id;
    info.state = fault == ES_RT_OK ? state : static_cast<std::uint32_t>(ES_RT_FAILED);
    info.fault = fault;
    info.frames = raw.frames;
    info.ticks = raw.ticks;
    info.native_frames = raw.native_frames;
    info.last_sequence = lastSequence;
    info.requested_frame = requestedFrame;
    info.last_latched_sequence = latchedSequence;
    info.latch_frame = latchFrame;
    info.latch_tick = latchTick;
    info.requested = requested;
    info.applied = applied;
    info.measured_rpm = raw.measured_rpm;
    info.cycle_rad = raw.cycle_rad;
    info.plate_angle_rad = raw.plate_angle_rad;
    info.load_reaction_nm = raw.load_reaction_nm;
    info.drive_reaction_nm = raw.drive_reaction_nm;
    info.gain = gain;
    info.gate = state == ES_RT_VACANT ? 0. : gate;
    return info;
}

void returnCell(Cell &cell) noexcept {
    cell.finalInfo = cell.voice->laneInfo(cell.identity, ES_RT_VACANT);
    cell.state.store(CellState::returned, std::memory_order_release);
}

}

namespace {
namespace fs = std::filesystem;
namespace assembly = engine_sim::assembly;
namespace json = engine_sim::json_runtime;
using engine_sim::fleet::CellState;
using engine_sim::fleet::Voice;
using engine_sim::fleet::finiteRange;
using engine_sim::fleet::prefix;
using engine_sim::fleet::preparedCell;
using engine_sim::fleet::validVector;
using engine_sim::config_v1::Diagnostic;

std::int32_t report(es_rt_diagnostic *detail, std::int32_t code, std::uint32_t stage,
                    std::string_view text) noexcept {
    if (detail) {
        detail->stage = stage;
        detail->code = static_cast<std::uint32_t>(code);
        engine_sim::api::copy(detail->message, text);
    }
    return code;
}

template<class Operation>
std::int32_t boundary(es_rt_diagnostic *detail, const Operation &operation) noexcept {
    if (detail) {
        const auto status = prefix(*detail);
        if (status != ES_RT_OK) return status;
    }
    report(detail, ES_RT_OK, ES_RT_DIAG_NONE, {});
    try {
        return operation();
    } catch (const std::bad_alloc &) {
        return report(detail, ES_RT_MEMORY, ES_RT_DIAG_PREPARE, "Fleet allocation failed");
    } catch (const fs::filesystem_error &) {
        return report(detail, ES_RT_RESOURCE, ES_RT_DIAG_RESOURCE, "Fleet filesystem operation failed");
    } catch (const engine_sim::SimulationFault &failure) {
        return report(detail, ES_RT_MODEL, ES_RT_DIAG_PREPARE, failure.what());
    } catch (...) {
        return report(detail, ES_RT_INTERNAL, ES_RT_DIAG_PREPARE, "Unexpected fleet error");
    }
}

bool absolute_path(const char *text, fs::path &out) {
    if (!text || !engine_sim::api::utf8(text)) return false;
    out = fs::u8path(text);
    return out.is_absolute();
}

bool supported_layout() noexcept {
    const std::uint16_t endian = 1;
    return sizeof(void *) == 8 && sizeof(double) == 8 && sizeof(float) == 4 &&
        std::numeric_limits<double>::is_iec559 && std::numeric_limits<float>::is_iec559 &&
        *reinterpret_cast<const unsigned char *>(&endian) == 1;
}

bool maximum_for_profile(const es_rt_config &value) noexcept {
    const auto frames = value.max_frames;
    return frames == 64 || frames == 128 ||
        (value.profile == ES_RT_BALANCED && (frames == 256 || frames == 512 || frames == 1024));
}

std::int32_t validate_voice(const es_rt_voice_config &config, es_rt_diagnostic *detail) noexcept {
    if (const auto status = prefix(config); status != ES_RT_OK)
        return report(detail, status, ES_RT_DIAG_ABI, "Unsupported fleet voice record prefix");
    if (config.reserved != 0 || config.mode > ES_RT_DRIVEN || config.controls_policy > ES_RT_RAW)
        return report(detail, ES_RT_INVALID, ES_RT_DIAG_INPUT, "Invalid fleet voice mode, controls policy or reserved field");
    if ((config.simulation_hz != 0 && (config.simulation_hz < 4000 || config.simulation_hz > 96000)) ||
        !finiteRange(config.starter_torque_nm, 10000))
        return report(detail, ES_RT_INVALID, ES_RT_DIAG_INPUT, "Invalid simulation rate or starter torque");
    const auto &initial = config.initial;
    if (!validVector(initial))
        return report(detail, ES_RT_INVALID, ES_RT_DIAG_INPUT, "Invalid initial physical vector");
    if ((config.mode == ES_RT_DRIVEN && (initial.starter || config.starter_torque_nm != 0)) ||
        (config.mode == ES_RT_FREE && initial.driven_rpm != 0))
        return report(detail, ES_RT_UNSUPPORTED, ES_RT_DIAG_INPUT, "Initial vector is incompatible with the voice mode");
    return ES_RT_OK;
}

std::int32_t rejected(es_rt_diagnostic *detail, const Diagnostic &failure) {
    const auto stage = failure.stage == "resources" ? ES_RT_DIAG_RESOURCE : ES_RT_DIAG_PREPARE;
    auto code = stage == ES_RT_DIAG_RESOURCE ? ES_RT_RESOURCE : ES_RT_MODEL;
    if (failure.code == "CFG_LIMIT" || failure.code == "API_CAPACITY") code = ES_RT_CAPACITY;
    else if (failure.code == "CFG_VERSION") code = ES_RT_VERSION;
    else if (failure.code == "API_INVALID") code = ES_RT_INVALID;
    std::string location = failure.pointer;
    if (failure.component_id) location += " [" + *failure.component_id + ']';
    if (!location.empty()) location += ": ";
    return report(detail, code, stage, location + failure.message);
}

template<class Prepare>
std::int32_t prepare_voice(es_rt_fleet *fleet, const es_rt_voice_config *voice, es_rt_token *out,
                           es_rt_diagnostic *detail, const Prepare &prepare) noexcept {
    return boundary(detail, [&]() -> std::int32_t {
        if (!fleet || !voice || !out)
            return report(detail, ES_RT_INVALID, ES_RT_DIAG_INPUT, "Fleet, voice settings and token destination are required");
        if (fleet->producer.phase.load(std::memory_order_relaxed) == engine_sim::fleet::FleetPhase::stopped)
            return report(detail, ES_RT_STATE, ES_RT_DIAG_PREPARE, "Fleet epoch is quiesced");
        if (const auto status = validate_voice(*voice, detail); status != ES_RT_OK) return status;
        if (fleet->prepared == 2)
            return report(detail, ES_RT_FULL, ES_RT_DIAG_PREPARE, "Both fleet candidate credits are occupied");
        std::size_t index = 0;
        for (; index < fleet->cellCount; ++index) {
            const auto &cell = fleet->cells[index];
            if (cell.state.load(std::memory_order_relaxed) == CellState::free &&
                cell.generation.load(std::memory_order_relaxed) != std::numeric_limits<std::uint64_t>::max()) break;
        }
        if (index == fleet->cellCount)
            return report(detail, ES_RT_CAPACITY, ES_RT_DIAG_PREPARE, "No reusable fleet cell generation remains");
        es_config options{sizeof(es_config), ES_ABI_V2, fleet->config.rate,
            voice->mode == ES_RT_FREE ? ES_FREE : ES_DRIVEN, voice->seed, voice->simulation_hz, 0, voice->starter_torque_nm};
        assembly::FleetCapacity capacity;
        auto result = prepare(options, capacity);
        if (const auto *error = std::get_if<Diagnostic>(&result)) return rejected(detail, *error);
        if (!assembly::charge_capacity(capacity.retained_bytes, 1, sizeof(Voice), assembly::fleet_cell_limit))
            return report(detail, ES_RT_CAPACITY, ES_RT_DIAG_PREPARE, "Voice owner exceeds the retained cell byte limit");
        auto candidate = std::make_unique<Voice>(options, *voice, capacity,
            std::get<json::PreparedModel>(std::move(result)));
        const auto &initial = voice->initial;
        if (voice->mode == ES_RT_DRIVEN && initial.driven_rpm > candidate->drivenLimitRpm)
            return report(detail, ES_RT_INVALID, ES_RT_DIAG_INPUT, "Initial driven RPM exceeds the prepared engine limit");
        candidate->runtime.setControls({sizeof(es_controls), ES_ABI_V2, initial.ignition, initial.starter,
                                       initial.demand, initial.load_nm, initial.driven_rpm});
        auto &cell = fleet->cells[index];
        const auto generation = cell.generation.load(std::memory_order_relaxed) + 1;
        cell.voice = std::move(candidate);
        cell.generation.store(generation, std::memory_order_relaxed);
        cell.state.store(CellState::prepared, std::memory_order_release);
        ++fleet->prepared;
        *out = {fleet->config.epoch, generation, static_cast<std::uint32_t>(index), 0};
        return ES_RT_OK;
    });
}

Diagnostic invalid_path(const char *message) {
    Diagnostic result;
    result.code = "API_INVALID";
    result.stage = "api";
    result.message = message;
    return result;
}
}

extern "C" {
std::uint32_t ES_RT_CALL es_rt_api_version() { return ES_RT_V1; }

std::int32_t ES_RT_CALL es_rt_create(const es_rt_config *input, es_rt_fleet **out, es_rt_diagnostic *detail) {
    return boundary(detail, [&]() -> std::int32_t {
        if (!input || !out) return report(detail, ES_RT_INVALID, ES_RT_DIAG_INPUT, "Fleet configuration and owner destination are required");
        if (const auto status = prefix(*input); status != ES_RT_OK)
            return report(detail, status, ES_RT_DIAG_ABI, "Unsupported fleet configuration prefix");
        auto config = *input;
        if (config.profile == 0) config.profile = ES_RT_BALANCED;
        if (config.profile == ES_RT_BALANCED && config.max_frames == 0) config.max_frames = 256;
        const auto cells = assembly::fleet_cells(config.lanes);
        if (!cells || !config.epoch || (config.profile != ES_RT_BALANCED && config.profile != ES_RT_INTERACTIVE) ||
            !maximum_for_profile(config))
            return report(detail, ES_RT_INVALID, ES_RT_DIAG_PROFILE, "Invalid fleet epoch, profile, lane count or quantum");
        if (!supported_layout() || (config.rate != 44100 && config.rate != 48000) ||
            config.flags != ES_RT_EXPERIMENTAL || config.qualification_id != 0)
            return report(detail, ES_RT_UNSUPPORTED, ES_RT_DIAG_PROFILE, "Fleet profile has no admitted qualification on this build");
        auto retained = sizeof(es_rt_fleet);
        if (!assembly::charge_capacity(retained, *cells, sizeof(engine_sim::fleet::Cell), assembly::fleet_storage_limit) ||
            !assembly::charge_capacity(retained, engine_sim::fleet::PcmTransport::slotCount(config),
                    sizeof(engine_sim::fleet::PcmTile), assembly::fleet_storage_limit))
            return report(detail, ES_RT_CAPACITY, ES_RT_DIAG_PROFILE, "Fixed fleet storage exceeds capacity");
        auto owner = std::make_unique<es_rt_fleet>(config, *cells);
        if (!owner->commands.lockFree() || !owner->pcm.lockFree() ||
            !owner->producer.phase.is_lock_free() || !owner->producer.generated.is_lock_free() ||
            !owner->snapshots.lockFree())
            return report(detail, ES_RT_UNSUPPORTED, ES_RT_DIAG_PROFILE, "Fleet transport atomics are not lock-free");
        for (std::size_t index = 0; index < *cells; ++index) {
            const auto &cell = owner->cells[index];
            if (!cell.state.is_lock_free() || !cell.generation.is_lock_free())
                return report(detail, ES_RT_UNSUPPORTED, ES_RT_DIAG_PROFILE, "Fleet cell atomics are not lock-free");
        }
        *out = owner.release();
        return ES_RT_OK;
    });
}

std::int32_t ES_RT_CALL es_rt_prepare_file(es_rt_fleet *fleet, const es_rt_voice_config *voice, const char *entry,
                                          const char *root, es_rt_token *out, es_rt_diagnostic *detail) {
    return prepare_voice(fleet, voice, out, detail, [&](const es_config &options, assembly::FleetCapacity &capacity)
            -> json::PreparedModelResult {
        fs::path entry_path, root_path;
        if (!absolute_path(entry, entry_path) || !absolute_path(root, root_path))
            return invalid_path("Absolute UTF-8 entry and resource paths are required");
        return json::prepare_fleet_file(entry_path, root_path, options, capacity);
    });
}

std::int32_t ES_RT_CALL es_rt_prepare_bytes(es_rt_fleet *fleet, const es_rt_voice_config *voice, const std::uint8_t *bytes,
                                           std::uint64_t length, const char *root, es_rt_token *out, es_rt_diagnostic *detail) {
    return prepare_voice(fleet, voice, out, detail, [&](const es_config &options, assembly::FleetCapacity &capacity)
            -> json::PreparedModelResult {
        fs::path root_path;
        if (!absolute_path(root, root_path)) return invalid_path("An absolute UTF-8 resource root is required");
        return json::prepare_fleet_bytes(reinterpret_cast<const char *>(bytes), length, root_path, options, capacity);
    });
}

std::int32_t ES_RT_CALL es_rt_discard(es_rt_fleet *fleet, es_rt_token token) {
    if (!fleet || token.reserved) return ES_RT_INVALID;
    auto *cell = preparedCell(*fleet, token);
    if (!cell) return ES_RT_STALE;
    cell->voice.reset();
    --fleet->prepared;
    cell->state.store(CellState::free, std::memory_order_release);
    return ES_RT_OK;
}

std::int32_t ES_RT_CALL es_rt_collect(es_rt_fleet *fleet, es_rt_token *out, es_rt_lane_info *info) {
    using namespace engine_sim::fleet;
    if (!fleet || !out || !info) return ES_RT_INVALID;
    for (std::size_t i = 0; i < fleet->cellCount; ++i) {
        auto &cell = fleet->cells[i];
        if (cell.state.load(std::memory_order_acquire) != CellState::returned) continue;
        *out = {fleet->config.epoch, cell.generation.load(std::memory_order_relaxed), static_cast<std::uint32_t>(i), 0};
        *info = cell.finalInfo;
        cell.voice.reset();
        cell.state.store(CellState::free, std::memory_order_release);
        return ES_RT_OK;
    }
    return ES_RT_EMPTY;
}

std::int32_t ES_RT_CALL es_rt_start(es_rt_fleet *fleet, es_rt_diagnostic *detail) {
    return boundary(detail, [&]() -> std::int32_t {
        if (!fleet) return report(detail, ES_RT_INVALID, ES_RT_DIAG_INPUT, "Fleet is required");
        const auto status = fleet->producer.start(*fleet);
        if (status != ES_RT_OK)
            return report(detail, status, ES_RT_DIAG_PROFILE, "Fleet producer could not prime the requested epoch");
        return ES_RT_OK;
    });
}

void ES_RT_CALL es_rt_destroy(es_rt_fleet *fleet) {
    if (fleet && es_rt_quiesce(fleet) == ES_RT_OK) delete fleet;
}
}
