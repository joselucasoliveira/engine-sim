#ifndef ENGINE_SIM_PRIVATE_LEGACY_RUNTIME_HPP
#define ENGINE_SIM_PRIVATE_LEGACY_RUNTIME_HPP

#include "prepared_engine.h"
#include "runtime.hpp"

namespace engine_sim::legacy_runtime {
struct Recipe {
    es_script::PreparationContext paths;
    PreparationConfig config;
    std::vector<es_script::ScriptInput> inputs;
};

// The tool and historical fixtures own script preparation outside the C SDK.
struct Access {
    static void load(Runtime &runtime, const es_script::PreparationContext &paths);
    static void reset(Runtime &runtime);
    static void adopt(Runtime &runtime, PreparedEngine engine, PullEngine::ControlLatch controls = {});
    static const Recipe *saved(const Runtime &runtime);
};

inline constexpr es_status script_error = 5;
es_status load(es_runtime *runtime, const char *entry, const char *library, const char *assets, es_error *error) noexcept;
es_status reset(es_runtime *runtime, es_error *error) noexcept;
}
#endif
