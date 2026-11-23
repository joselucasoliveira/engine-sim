#ifndef ENGINE_SIM_LEGACY_SHA256_HPP
#define ENGINE_SIM_LEGACY_SHA256_HPP

#include <filesystem>
#include <string>
#include <string_view>

namespace engine_sim::legacy::cli {
std::string sha256(std::string_view bytes);
std::string sha256_file(const std::filesystem::path &file);
}
#endif
