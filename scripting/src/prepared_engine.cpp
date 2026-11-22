#include "../include/prepared_engine.h"
#include "../../include/wave_reader.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>

namespace engine_sim {
namespace {
namespace fs = std::filesystem;

void require(bool condition, const std::string &field, const fs::path &path) {
    if (!condition) throw EngineLoadError({"model", path.u8string(), field});
}

void validateGeometry(Engine &engine, const fs::path &entry) {
    const int cylinders = engine.getCylinderCount();
    require(cylinders > 0 && cylinders <= std::numeric_limits<int>::max() / 8, "cylinder count overflow", entry);
    require(engine.getCrankshaftCount() > 0 && engine.getCylinderBankCount() > 0
        && engine.getIntakeCount() > 0 && engine.getExhaustSystemCount() > 0, "incomplete topology", entry);
    for (int i = 0; i < cylinders; ++i) {
        const auto &piston = *engine.getPiston(i);
        const auto &rod = *piston.getRod();
        const auto &bank = *piston.getCylinderBank();
        require(bank.getIndex() >= 0 && bank.getIndex() < engine.getCylinderBankCount(),
            "cylinder bank index", entry);
        const auto &head = *engine.getHead(bank.getIndex());
        require(piston.getCylinderIndex() >= 0 && piston.getCylinderIndex() < bank.getCylinderCount(),
            "piston cylinder index", entry);
        const auto &exhaust = *head.getExhaustSystem(piston.getCylinderIndex());
        const double delaySamples = (head.getHeaderPrimaryLength(piston.getCylinderIndex()) +
            exhaust.getLength()) * 10000.0 / 343.0;
        require(std::isfinite(delaySamples) && delaySamples > 0 &&
            delaySamples < std::numeric_limits<int>::max() - 32.0, "exhaust delay extent", entry);
        for (int sample = 0; sample < 1000; ++sample) {
            const double angle = 2 * constants::pi * sample / 1000;
            double x, y, orientation, travel;
            require(placeRod(rod, bank, *rod.getCrankshaft(), angle, &x, &y, &orientation, &travel),
                "cylinder " + std::to_string(i) + " has unusable rod geometry", entry);
            const double volume = bank.boreSurfaceArea() *
                (bank.getDeckHeight() - travel - piston.getCompressionHeight()) +
                head.getCombustionChamberVolume() - piston.getDisplacement();
            require(std::isfinite(volume) && volume > 0,
                "cylinder " + std::to_string(i) + " has nonpositive chamber volume", entry);
        }
    }
}

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
}

PreparedEngine prepareEngine(const es_script::PreparationContext &recipe, PreparationConfig config,
        const std::vector<es_script::ScriptInput> *expectedInputs) {
    if (config.mode != ES_FREE && config.mode != ES_DRIVEN) {
        throw EngineLoadError({"unsupported", recipe.entry.u8string(), "Unknown engine mode"});
    }
    if (!std::isfinite(config.starterTorqueNm) || config.starterTorqueNm < 0 || config.starterTorqueNm > 10000 ||
        (config.simulationHz != 0 && (config.simulationHz < 4000 || config.simulationHz > 96000))) {
        throw EngineLoadError({"configuration", recipe.entry.u8string(), "Invalid frequency or starter torque"});
    }
    if (config.mode == ES_DRIVEN && config.starterTorqueNm != 0) {
        throw EngineLoadError({"unsupported", recipe.entry.u8string(), "Starter torque is unavailable in driven mode"});
    }
    PreparedEngine candidate;
    candidate.recipe = recipe;
    candidate.config = config;
    std::set<fs::path> observed;
    const auto verify = [&](const es_script::ScriptInput &input) {
        if (expectedInputs != nullptr) {
            const auto found = std::find_if(expectedInputs->begin(), expectedInputs->end(), [&](const auto &expected) {
                return expected.resolvedPath == input.resolvedPath;
            });
            if (found == expectedInputs->end() || found->bytes != input.bytes) {
                throw EngineLoadError({"resources", input.resolvedPath.u8string(), "Preparation input changed"});
            }
        }
        observed.insert(input.resolvedPath);
    };
    auto definition = es_script::Compiler::prepare(recipe, candidate.inputs, verify);
    auto &engine = *definition.engine;
    engine.setRandomSeed(config.seed);
    const double frequency = config.simulationHz == 0 ? engine.getSimulationFrequency() : config.simulationHz;
    require(std::isfinite(frequency) && frequency >= 4000 && frequency <= 96000 &&
        std::floor(frequency) == frequency, "simulation frequency must be an integer in [4000,96000]", recipe.entry);
    require(std::isfinite(engine.getRedline()) && engine.getRedline() > 0, "invalid engine redline", recipe.entry);
    validateGeometry(engine, recipe.entry);
    for (const auto value : {engine.getInitialJitter(), engine.getInitialNoise(), engine.getInitialHighFrequencyGain()}) {
        require(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(),
            "synthesizer parameter exceeds float range", recipe.entry);
    }
    const EngineSessionOptions options{static_cast<int>(frequency), units::torque(config.starterTorqueNm, units::Nm)};
    candidate.session = EngineSession::prepare(std::move(definition), [&](const ImpulseResponse &response) {
        const auto path = fs::u8path(response.getFilename());
        require(std::isfinite(response.getVolume()) && std::abs(response.getVolume()) <= std::numeric_limits<float>::max(),
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
            require(samples.size() <= std::numeric_limits<unsigned>::max(), "IR sample count overflow", path);
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
}
