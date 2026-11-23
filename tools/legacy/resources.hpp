#ifndef ENGINE_SIM_LEGACY_RESOURCES_HPP
#define ENGINE_SIM_LEGACY_RESOURCES_HPP

#include "snapshot.hpp"

#include <engine_sim/config-validation.hpp>

namespace engine_sim::legacy {

struct PinnedWave {
    std::filesystem::path canonical_path;
    std::string relative_path;
    std::vector<unsigned char> bytes;
    std::size_t samples{};
    static constexpr int channels = 1;
    static constexpr int rate = 44100;
    static constexpr int encoding_bits = 16;
};

struct ResolvedImpulse {
    ImpulseOrigin origin;
    std::size_t wave{};
};

struct ResolvedSnapshot {
    config_v1::NormalizedDocument model;
    std::filesystem::path resource_root;
    std::vector<ResolvedImpulse> impulses;
    std::vector<PinnedWave> waves;
    std::vector<IgnoredInput> ignored_inputs;
    std::vector<es_script::ScriptInput> consumed_scripts;
};

using ResolveResult = std::variant<ResolvedSnapshot, config_v1::Diagnostic>;

// Rebase evaluator-selected resources beneath the absolute root, normalize the
// effective graph, then pin and decode each selected WAVE from the same bytes.
ResolveResult resolve(Snapshot input, const std::filesystem::path &root);

}
#endif
