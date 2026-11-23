#include "../include/prepared_engine.h"
#include "../../include/wave_reader.h"
#include "../../src/runtime/concrete_validation.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>

namespace engine_sim {
namespace {
namespace fs = std::filesystem;

std::string readImpulseBytes(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw EngineLoadError({"resources", path.u8string(), "Cannot open impulse response"});
    const auto length = stream.tellg();
    if (length < 12 || length > static_cast<std::streamoff>(maxImpulseWaveBytes)) {
        throw EngineLoadError({"resources", path.u8string(), "Impulse response size is outside WAVE limits"});
    }
    std::string bytes(static_cast<std::size_t>(length), '\0');
    stream.seekg(0);
    if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        throw EngineLoadError({"resources", path.u8string(), "Cannot read complete impulse response"});
    }
    return bytes;
}
void verifyInput(const es_script::ScriptInput &input, const std::vector<es_script::ScriptInput> *expectedInputs) {
    if (!expectedInputs) return;
    const auto found = std::find_if(expectedInputs->begin(), expectedInputs->end(), [&](const auto &expected) {
        return expected.resolvedPath == input.resolvedPath;
    });
    if (found == expectedInputs->end() || found->bytes != input.bytes)
        throw EngineLoadError({"resources", input.resolvedPath.u8string(), "Preparation input changed"});
}
void validateConfig(const PreparationConfig &config, const fs::path &entry) {
    if (config.mode != ES_FREE && config.mode != ES_DRIVEN)
        throw EngineLoadError({"unsupported", entry.u8string(), "Unknown engine mode"});
    if (config.desktop && config.mode != ES_FREE)
        throw EngineLoadError({"unsupported", entry.u8string(), "Desktop requires free-running mechanics"});
    if (!std::isfinite(config.starterTorqueNm) || config.starterTorqueNm < 0 || config.starterTorqueNm > 10000 ||
        (config.simulationHz != 0 && (config.simulationHz < (config.desktop ? 400U : 4000U)
                                  || config.simulationHz > (config.desktop ? 400000U : 96000U))))
        throw EngineLoadError({"configuration", entry.u8string(), "Invalid frequency or starter torque"});
    if (config.mode == ES_DRIVEN && config.starterTorqueNm != 0)
        throw EngineLoadError({"unsupported", entry.u8string(), "Starter torque is unavailable in driven mode"});
}
PreparedEngine finishPrepared(PreparedEngine candidate, EngineDefinition definition,
        const std::vector<es_script::ScriptInput> *expectedInputs) {
    if (!definition.engine) throw EngineLoadError({"graph", candidate.recipe.entry.u8string(), "No engine was selected"});
    const auto &recipe = candidate.recipe;
    const auto config = candidate.config;
    std::set<fs::path> observed;
    for (const auto &input : candidate.inputs) observed.insert(input.resolvedPath);
    const auto verify = [&](const es_script::ScriptInput &input) {
        verifyInput(input, expectedInputs);
        observed.insert(input.resolvedPath);
    };
    auto &engine = *definition.engine;
    engine.setRandomSeed(config.seed);
    const double declaredFrequency = config.simulationHz == 0 ? engine.getSimulationFrequency() : config.simulationHz;
    // A desktop can use fractional authored Hz. The physical clock uses its integer part.
    validateConcreteModel(engine, declaredFrequency, config.desktop, recipe.entry);
    const auto frequency = std::trunc(declaredFrequency);
    const EngineSessionOptions options{config.desktop ? 0 : static_cast<int>(frequency), units::torque(config.starterTorqueNm, units::Nm)};
    candidate.session = EngineSession::prepare(std::move(definition), [&](const ImpulseResponse &response) {
        const auto path = fs::u8path(response.getFilename());
        requireConcreteModel(std::isfinite(response.getVolume()) && std::abs(response.getVolume()) <= std::numeric_limits<float>::max(),
            "impulse gain exceeds float range", path);
        es_script::ScriptInput input;
        try {
            input.resolvedPath = fs::canonical(path);
            const auto prior = std::find_if(candidate.inputs.begin(), candidate.inputs.end(), [&](const auto &saved) {
                return saved.resolvedPath == input.resolvedPath;
            });
            input.bytes = prior == candidate.inputs.end() ? readImpulseBytes(path) : prior->bytes;
        }
        catch (const fs::filesystem_error &error) {
            throw EngineLoadError({"resources", path.u8string(), error.what()});
        }
        verify(input);
        try {
            const std::vector<unsigned char> bytes(input.bytes.begin(), input.bytes.end());
            auto samples = decodeImpulseWave(bytes, path);
            requireConcreteModel(samples.size() <= std::numeric_limits<unsigned>::max(), "IR sample count overflow", path);
            if (std::none_of(candidate.inputs.begin(), candidate.inputs.end(), [&](const auto &prior) {
                return prior.resolvedPath == input.resolvedPath;
            })) candidate.inputs.push_back(std::move(input));
            return samples;
        }
        catch (const WaveError &error) {
            throw EngineLoadError({"resources", path.u8string(), error.reason});
        }
    }, options);
    if (expectedInputs != nullptr && observed.size() != expectedInputs->size()) {
        throw EngineLoadError({"resources", recipe.entry.u8string(), "Preparation input set changed"});
    }
    return candidate;
}
} // namespace

PreparedEngine prepareEngine(const es_script::PreparationContext &recipe, PreparationConfig config,
        const std::vector<es_script::ScriptInput> *expectedInputs) {
    validateConfig(config, recipe.entry);
    PreparedEngine candidate;
    candidate.recipe = recipe; candidate.config = config;
    auto definition = es_script::Compiler::prepare(recipe, candidate.inputs,
        [&](const es_script::ScriptInput &input) { verifyInput(input, expectedInputs); });
    return finishPrepared(std::move(candidate), std::move(definition), expectedInputs);
}

}
