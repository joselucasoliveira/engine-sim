#ifndef ENGINE_SIM_PRIVATE_JSON_SESSION_HPP
#define ENGINE_SIM_PRIVATE_JSON_SESSION_HPP

#include "engine_session.h"
#include <engine_sim/config-v1.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>
#include <variant>

namespace engine_sim {
struct JsonRecipe;

namespace json_runtime {
struct PreparedModel {
    std::unique_ptr<EngineSession> session;
    std::shared_ptr<const JsonRecipe> recipe;
};
using PreparedModelResult = std::variant<PreparedModel, config_v1::Diagnostic>;
using InputVisitor = std::function<void(const std::filesystem::path &, std::string_view)>;

PreparedModelResult prepare_desktop(const std::filesystem::path &entry,
                                    const std::filesystem::path &root, std::uint64_t seed);
PreparedModelResult reset_desktop(const JsonRecipe &saved, std::uint64_t seed);
PreparedModel require_model(PreparedModelResult result, const std::filesystem::path &entry = {});
void visit_file_inputs(const JsonRecipe &recipe, const InputVisitor &visitor);
}
}
#endif
