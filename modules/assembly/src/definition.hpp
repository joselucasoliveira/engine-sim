#ifndef ENGINE_SIM_ASSEMBLY_DEFINITION_HPP
#define ENGINE_SIM_ASSEMBLY_DEFINITION_HPP

#include "engine_session.h"

#include <engine_sim/config-validation.hpp>

#include <cstdint>
#include <string>
#include <variant>

namespace engine_sim::assembly {

using DefinitionResult = std::variant<EngineDefinition, config_v1::Diagnostic>;

// Assemble a detached definition and perform the legacy geometry checks.
// Resource locators stay relative. The session owner supplies pinned samples
// and the effective clock profile when it activates the definition.
DefinitionResult build_definition(const config_v1::NormalizedDocument &model,
                                  std::uint64_t seed, std::string source = {});

}
#endif
