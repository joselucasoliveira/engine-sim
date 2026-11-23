#ifndef ENGINE_SIM_ASSEMBLY_MECHANICAL_HPP
#define ENGINE_SIM_ASSEMBLY_MECHANICAL_HPP

#include "engine.h"
#include <engine_sim/config-validation.hpp>

#include <functional>
#include <memory>
#include <variant>

namespace engine_sim::assembly {

// Private preparation phases. Observers can inject failures without making a
// partially constructed engine available to a caller.
enum class MechanicalPhase { engine, crankshaft, bank, cylinder, links, geometry };
using MechanicalCheckpoint = std::function<void(MechanicalPhase, std::size_t)>;

struct MechanicalCandidate {
    config_v1::NormalizedDocument model;
    std::unique_ptr<Engine> engine;
};

using MechanicalResult = std::variant<MechanicalCandidate, config_v1::Diagnostic>;

// Construct and check mechanical geometry before any simulator is activated.
MechanicalResult build_mechanics(config_v1::NormalizedDocument model,
                                 const MechanicalCheckpoint &checkpoint = {});

}
#endif
