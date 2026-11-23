#ifndef ENGINE_SIM_PRIVATE_JSON_SESSION_HPP
#define ENGINE_SIM_PRIVATE_JSON_SESSION_HPP

#include "engine_session.h"
#include <engine_sim/config-v1.hpp>

#include <cstdint>
#include <filesystem>
#include <variant>

namespace engine_sim {
struct JsonRecipe;

namespace json_runtime {
struct PreparedModel {
    std::unique_ptr<EngineSession> session;
    std::shared_ptr<const JsonRecipe> recipe;
};
using PreparedModelResult = std::variant<PreparedModel, config_v1::Diagnostic>;

PreparedModelResult prepare_desktop(const std::filesystem::path &entry,
                                    const std::filesystem::path &root, std::uint64_t seed);
PreparedModelResult reset_desktop(const JsonRecipe &saved, std::uint64_t seed);
}
}
#endif
