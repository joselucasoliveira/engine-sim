#include "preparation.hpp"

#include "wave_reader.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace engine_sim::assembly {
namespace {
namespace cfg = config_v1;
namespace fs = std::filesystem;

cfg::Diagnostic diagnostic(std::string_view code, std::string_view stage, std::string_view message,
                           std::string path, std::string pointer) {
    cfg::Diagnostic result;
    result.code = code;
    result.stage = stage;
    result.pointer = std::move(pointer);
    result.message = message;
    if (!path.empty()) result.path = std::move(path);
    return result;
}

cfg::Diagnostic resource_error(std::string_view code, const fs::path &path, std::string_view message,
                               const std::vector<cfg::ImpulseResponse> &impulses, std::size_t index) {
    auto result = diagnostic(code, "resources", message, path.u8string(),
                             "/components/impulses/" + std::to_string(index) + "/path");
    result.component_id = impulses[index].id.value;
    result.field = "path";
    return result;
}

bool portable_component(std::string_view name) {
    if (name.empty() || name == "." || name == ".." || name.back() == ' ' || name.back() == '.')
        return false;
    if (std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }) ||
        name.find_first_of("<>:\"/\\|?*") != std::string_view::npos)
        return false;

    std::string basename(name.substr(0, name.find('.')));
    for (auto &c : basename) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    if (basename == "CON" || basename == "PRN" || basename == "AUX" || basename == "NUL" ||
        basename == "CONIN$" || basename == "CONOUT$") return false;
    const auto prefix = std::string_view(basename).substr(0, 3);
    if (prefix == "COM" || prefix == "LPT") {
        const auto suffix = std::string_view(basename).substr(3);
        if ((suffix.size() == 1 && suffix[0] >= '1' && suffix[0] <= '9') ||
            suffix == "\xc2\xb9" || suffix == "\xc2\xb2" || suffix == "\xc2\xb3") return false;
    }
    return true;
}

bool portable_locator(std::string_view locator) {
    auto remaining = locator;
    for (;;) {
        const auto separator = remaining.find('/');
        if (!portable_component(remaining.substr(0, separator))) return false;
        if (separator == std::string_view::npos) return true;
        remaining.remove_prefix(separator + 1);
    }
}

bool contained(const fs::path &root, const fs::path &file) {
    const auto end = file.end();
    const auto prefix = std::mismatch(root.begin(), root.end(), file.begin(), end);
    return prefix.first == root.end() && prefix.second != end;
}

using OptionsResult = std::variant<EngineSessionOptions, cfg::Diagnostic>;

OptionsResult options_for(const cfg::Engine &engine, const PreparationProfile &profile,
                          const std::string &source) {
    const auto invalid = [&](std::string_view message, const char *pointer) {
        return diagnostic("CFG_INITIALIZE", "initialize", message, source, pointer);
    };
    if (profile.mode != Mode::free && profile.mode != Mode::driven && profile.mode != Mode::desktop)
        return invalid("Unknown engine mode", "");
    if (!std::isfinite(profile.starter_torque_nm) || profile.starter_torque_nm < 0 ||
        profile.starter_torque_nm > 10000)
        return invalid("Starter torque override must be in [0,10000] N m", "");
    if (profile.mode == Mode::driven && profile.starter_torque_nm != 0)
        return invalid("Starter torque is unavailable in driven mode", "");

    const bool desktop = profile.mode == Mode::desktop;
    const double declared = profile.simulation_hz == 0 ? engine.simulation_frequency.value : profile.simulation_hz;
    const double effective = desktop ? std::trunc(declared) : declared;
    if (!std::isfinite(effective) || effective < (desktop ? 400 : 4000) ||
        effective > (desktop ? 400000 : 96000) || std::floor(effective) != effective)
        return invalid(desktop ? "Effective desktop simulation frequency must be in [400,400000]"
                               : "Simulation frequency must be an integer in [4000,96000]", "/engine/simulation_frequency");
    return EngineSessionOptions{static_cast<int>(effective), units::torque(profile.starter_torque_nm, units::Nm), desktop};
}

using PinResult = std::variant<ResourceInputs, cfg::Diagnostic>;

PinResult pin(const std::vector<cfg::ImpulseResponse> &impulses, const fs::path &requested,
              const ResourceInputs *expected, const PreparationCheckpoint &checkpoint, bool fleet) {
    const auto root_error = [&](std::string_view code, std::string_view reason) {
        return diagnostic(code, "resources", reason, requested.u8string(), "");
    };
    if (!requested.is_absolute() ||
        requested.native().find(fs::path::value_type{}) != fs::path::string_type::npos)
        return root_error("CFG_RESOURCE_PATH", "Resource root must be an absolute directory");
    std::error_code error;
    ResourceInputs inputs;
    inputs.root = fs::canonical(requested, error);
    if (error || !fs::is_directory(inputs.root, error) || error)
        return root_error("CFG_RESOURCE_PATH", "Resource root must resolve to an existing directory");
    if (expected && (expected->root != inputs.root || expected->bindings.size() != impulses.size()))
        return root_error("CFG_RESOURCE_BYTES", "Resource root or locator set changed");

    inputs.bindings.reserve(impulses.size());
    std::unordered_map<fs::path, std::size_t> by_path;
    std::unordered_map<std::string_view, std::size_t> by_locator;
    std::size_t remaining = fleet_wave_limit;
    for (std::size_t i = 0; i < impulses.size(); ++i) {
        const auto &impulse = impulses[i];
        if (!portable_locator(impulse.path)) {
            auto failure = resource_error("CFG_RESOURCE_PATH", {}, "Impulse locator is not portable", impulses, i);
            failure.path = impulse.path;
            return failure;
        }
        const auto path = inputs.root / fs::u8path(impulse.path);

        const auto cached = by_locator.find(impulse.path);
        const auto actual = cached == by_locator.end() ? fs::canonical(path, error) : inputs.files[cached->second].path;
        if (error || !contained(inputs.root, actual) || !fs::is_regular_file(actual, error) || error)
            return resource_error("CFG_RESOURCE_PATH", path, "Impulse requires a regular file below the resource root", impulses, i);

        if (expected) {
            const auto &binding = expected->bindings[i];
            if (binding.locator != impulse.path || binding.input >= expected->files.size() ||
                expected->files[binding.input].path != actual)
                return resource_error("CFG_RESOURCE_BYTES", path, "Resolved impulse identity changed", impulses, i);
        }
        const auto [slot, inserted] = by_path.try_emplace(actual, inputs.files.size());
        if (inserted) {
            try {
                auto bytes = fleet ? readImpulseWaveBytes(actual, remaining) : readImpulseWaveBytes(actual);
                if (expected && bytes != expected->files[expected->bindings[i].input].bytes)
                    return resource_error("CFG_RESOURCE_BYTES", actual, "Impulse input bytes changed", impulses, i);
                if (fleet) remaining -= bytes.size();
                inputs.files.push_back({actual, std::move(bytes)});
            } catch (const WaveError &problem) {
                if (problem.failure == WaveFailure::Budget) {
                    const auto budget = extent_error("unique WAVE bytes",
                        fleet_wave_limit - remaining + problem.requestedBytes, fleet_wave_limit);
                    return resource_error("CFG_LIMIT", actual, budget.message, impulses, i);
                }
                return resource_error("CFG_RESOURCE_BYTES", actual, problem.reason, impulses, i);
            }
            if (checkpoint) checkpoint(PreparationPhase::pin, slot->second);
        }
        by_locator.try_emplace(impulse.path, slot->second);
        inputs.bindings.push_back({impulse.path, slot->second});
    }
    if (expected && inputs.files.size() != expected->files.size())
        return root_error("CFG_RESOURCE_BYTES", "Resolved impulse set changed");
    return inputs;
}

}

PreparationResult prepare_session(const cfg::NormalizedDocument &model, const fs::path &absolute_root,
                                  PreparationProfile profile, std::string source,
                                  const ResourceInputs *expected, const PreparationCheckpoint &checkpoint) {
    auto options = options_for(model.values().engine, profile, source);
    if (auto *failure = std::get_if<cfg::Diagnostic>(&options)) return std::move(*failure);
    FleetCapacity capacity;
    const bool fleet = profile.fleet != nullptr;
    if (fleet) {
        capacity = profile.fleet->capacity;
        capacity.output_hz = profile.fleet->output_hz;
        capacity.simulation_hz = static_cast<std::uint32_t>(std::get<EngineSessionOptions>(options).simulationFrequency);
        if (profile.mode == Mode::desktop || (capacity.output_hz != 44100 && capacity.output_hz != 48000)) {
            auto problem = diagnostic("CFG_LIMIT", "initialize", "Fleet requires FREE or DRIVEN at 44100 or 48000 Hz", source, "");
            problem.field = profile.mode == Mode::desktop ? "mode" : "output_hz";
            return problem;
        }
        if (auto problem = check_fleet_model(model, capacity, profile.mode == Mode::driven)) {
            if (!source.empty()) problem->path = source;
            return std::move(*problem);
        }
    }
    auto built = build_definition(model, profile.seed, source);
    if (auto *failure = std::get_if<cfg::Diagnostic>(&built)) {
        if (!source.empty()) failure->path = std::move(source);
        return std::move(*failure);
    }
    if (checkpoint) checkpoint(PreparationPhase::definition, 0);
    const auto &definitions = model.values().components.impulses;
    auto pinned = pin(definitions, absolute_root, expected, checkpoint, fleet);
    if (auto *failure = std::get_if<cfg::Diagnostic>(&pinned)) return std::move(*failure);
    auto inputs = std::get<ResourceInputs>(std::move(pinned));

    std::vector<std::vector<std::int16_t>> samples(inputs.files.size());
    std::unordered_map<std::string_view, std::size_t> indices;
    for (std::size_t i = 0; i < inputs.bindings.size(); ++i) {
        const auto &[locator, input] = inputs.bindings[i];
        indices.try_emplace(locator, input);
        if (!samples[input].empty()) continue;
        const auto &file = inputs.files[input];
        try {
            samples[input] = decodeImpulseWave(file.bytes, file.path);
        } catch (const WaveError &problem) {
            return resource_error("CFG_RESOURCE_FORMAT", file.path, problem.reason, definitions, i);
        }
        if (samples[input].size() > std::numeric_limits<unsigned>::max())
            return resource_error("CFG_RESOURCE_BYTES", file.path, "Impulse sample count overflow", definitions, i);
        if (fleet && (samples[input].size() > file.bytes.size() / sizeof(std::int16_t) ||
                      !charge_capacity(capacity.decoded_sample_bytes, samples[input].capacity(),
                                       sizeof(std::int16_t), fleet_wave_limit)))
            return resource_error("CFG_LIMIT", file.path, "Decoded WAVE capacity exceeds admitted source bytes", definitions, i);
        if (checkpoint) checkpoint(PreparationPhase::decode, input);
    }

    auto definition = std::get<EngineDefinition>(std::move(built));
    if (fleet) {
        const auto &exhausts = model.values().components.exhausts;
        const auto &routes = model.topology().exhausts;
        for (std::size_t i = 0; i < routes.size(); ++i) {
            const auto *response = definition.engine->getExhaustSystem(static_cast<int>(i))->getImpulseResponse();
            const auto &decoded = samples[indices.at(response->getFilename())];
            const auto taps = Synthesizer::effectiveImpulseTaps(decoded.data(), static_cast<unsigned>(decoded.size()));
            if (auto problem = assign_taps(capacity, i, taps, exhausts[routes[i]].id.value)) {
                if (!source.empty()) problem->path = source;
                return std::move(*problem);
            }
        }
        if (!account_pinned(capacity, inputs) || !account_session(capacity, model)) {
            auto problem = extent_error("retained cell bytes", capacity.retained_bytes, fleet_cell_limit);
            if (!source.empty()) problem.path = source;
            return problem;
        }
    }
    std::size_t channel = 0;
    try {
        auto session = EngineSession::prepare(std::move(definition), [&](const ImpulseResponse &response)
                -> const std::vector<std::int16_t> & {
            if (checkpoint) checkpoint(PreparationPhase::channel, channel++);
            return samples[indices.at(response.getFilename())];
        }, std::get<EngineSessionOptions>(options));
        if (fleet && !verify_session(capacity, *session))
            return diagnostic("CFG_INITIALIZE", "initialize", "Fleet graph or synthesis capacities differ from preparation plan",
                              source, "/engine");
        if (checkpoint) checkpoint(PreparationPhase::session, 0);
        if (fleet) profile.fleet->capacity = capacity;
        return PreparedSession{std::move(inputs), std::move(session)};
    } catch (const EngineLoadError &problem) {
        return diagnostic("CFG_INITIALIZE", "initialize", problem.diagnostic.message, problem.diagnostic.path, "/engine");
    }
}

}
