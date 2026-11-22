#ifndef ENGINE_SIM_PREPARED_ENGINE_H
#define ENGINE_SIM_PREPARED_ENGINE_H

#include "compiler.h"
#include "../../include/engine_sim/runtime.h"
#include <cstdint>

namespace engine_sim {
struct PreparationConfig {
    std::uint32_t simulationHz = 0;
    std::uint32_t mode = ES_FREE;
    double starterTorqueNm = 0;
};

struct PreparedEngine {
    es_script::PreparationContext recipe;
    PreparationConfig config;
    std::vector<es_script::ScriptInput> inputs;
    std::unique_ptr<EngineSession> session;
};

PreparedEngine prepareEngine(const es_script::PreparationContext &recipe, PreparationConfig config = {},
    const std::vector<es_script::ScriptInput> *expectedInputs = nullptr);
}
#endif
