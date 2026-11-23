#ifndef ENGINE_SIM_PRIVATE_JSON_RUNTIME_HPP
#define ENGINE_SIM_PRIVATE_JSON_RUNTIME_HPP

#include <engine_sim/config-v1.hpp>
#include <engine_sim/runtime.h>

#include <filesystem>
#include <functional>

namespace engine_sim::assembly { struct FleetCapacity; }
namespace engine_sim::json_runtime {

enum class Phase { document, normalized, prepared, snapshot };
using Checkpoint = std::function<void(Phase)>;

es_status load_file(es_runtime *runtime, const std::filesystem::path &absolute_entry,
                    const std::filesystem::path &absolute_root, config_v1::Diagnostic *detail = nullptr,
                    es_error *error = nullptr, const Checkpoint &checkpoint = {}) noexcept;

es_status load_bytes(es_runtime *runtime, const char *bytes, std::uint64_t size,
                     const std::filesystem::path &absolute_root, config_v1::Diagnostic *detail = nullptr,
                     es_error *error = nullptr, const Checkpoint &checkpoint = {}) noexcept;

es_status reset(es_runtime *runtime, config_v1::Diagnostic *detail = nullptr, es_error *error = nullptr,
                const Checkpoint &checkpoint = {}) noexcept;

}
#endif
