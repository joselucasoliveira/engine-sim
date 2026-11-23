#ifndef ENGINE_SIM_TEST_LEGACY_RUNTIME_HPP
#define ENGINE_SIM_TEST_LEGACY_RUNTIME_HPP

#include "prepared_engine.h"
#include "runtime.hpp"

namespace engine_sim::legacy_runtime {
std::vector<es_script::ScriptInput> prepare(es_runtime &runtime, const es_script::PreparationContext &paths,
    const std::vector<es_script::ScriptInput> *expected = nullptr);
es_status load(es_runtime *runtime, const char *entry, const char *library, const char *assets, es_error *error) noexcept;
}
#endif
