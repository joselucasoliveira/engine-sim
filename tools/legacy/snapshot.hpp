#ifndef ENGINE_SIM_LEGACY_SNAPSHOT_HPP
#define ENGINE_SIM_LEGACY_SNAPSHOT_HPP

#include "compiler.h"
#include "legacy_curves.h"

#include <engine_sim/config-v1.hpp>

namespace engine_sim::legacy {

struct ImpulseOrigin {
    config_v1::Ref<config_v1::ImpulseResponseTag> impulse;
    std::filesystem::path declaring_unit;
    std::string evaluated_filename;
};

struct IgnoredInput {
    std::string component_id;
    std::string name;
    double value;
};

struct HarmonicOrigin {
    std::string function_id;
    legacy_curves::HarmonicInputs inputs;
};

struct Snapshot {
    config_v1::Document values;
    std::vector<ImpulseOrigin> impulses;
    std::vector<IgnoredInput> ignored_inputs;
    std::vector<HarmonicOrigin> harmonics;
    std::vector<es_script::ScriptInput> consumed_scripts;
};

// Captures the selected evaluated nodes inside the legacy execution scope.
// The result owns all data after destruction of the program and compiler.
Snapshot capture(const es_script::PreparationContext &source,
                 const es_script::Compiler::InputVerifier &verify = {});

}
#endif
