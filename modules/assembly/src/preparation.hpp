#ifndef ENGINE_SIM_ASSEMBLY_PREPARATION_HPP
#define ENGINE_SIM_ASSEMBLY_PREPARATION_HPP

#include "definition.hpp"

#include <filesystem>
#include <functional>
#include <vector>

namespace engine_sim::assembly {

enum class Mode { free, driven, desktop };

struct PreparationProfile {
    Mode mode = Mode::free;
    std::uint32_t simulation_hz = 0;
    double starter_torque_nm = 0.0;
    std::uint64_t seed = 1;
};

struct ResourceInput {
    std::filesystem::path path;
    std::vector<unsigned char> bytes;
};

struct ResourceBinding {
    std::string locator;
    std::size_t input;
};

struct ResourceInputs {
    std::filesystem::path root;
    std::vector<ResourceInput> files;
    // One entry per impulse definition in authored table order. Inputs are
    // deduplicated by resolved path, even when locators or gains differ.
    std::vector<ResourceBinding> bindings;
};

struct PreparedSession {
    ResourceInputs inputs;
    std::unique_ptr<EngineSession> session;
};

enum class PreparationPhase { definition, pin, decode, channel, session };
using PreparationCheckpoint = std::function<void(PreparationPhase, std::size_t)>;
using PreparationResult = std::variant<PreparedSession, config_v1::Diagnostic>;

// On reset, validate the complete identity set before decoding any WAVE.
// Source is optional diagnostic context. Mode-specific runtime controls and
// clock publication are owned by the adopter of the detached session.
PreparationResult prepare_session(const config_v1::NormalizedDocument &model,
                                  const std::filesystem::path &absolute_root,
                                  PreparationProfile profile,
                                  std::string source = {},
                                  const ResourceInputs *expected = nullptr,
                                  const PreparationCheckpoint &checkpoint = {});

}
#endif
