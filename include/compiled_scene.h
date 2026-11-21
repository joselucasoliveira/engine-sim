#ifndef ENGINE_SIM_COMPILED_SCENE_H
#define ENGINE_SIM_COMPILED_SCENE_H

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace engine_sim {
struct SceneMesh {
    std::string name, material;
    std::int32_t parent = -1;
    std::array<float, 3> position{}, eulerDegrees{}, scale{};
    std::array<float, 4> orientation{};
    std::vector<std::array<float, 10>> vertices;
    std::vector<std::uint16_t> indices;
};
struct CompiledScene { std::vector<SceneMesh> meshes; };

// Delta's bundled static geometry layout: explicit LE fields, never native struct reads.
CompiledScene readCompiledScene(const std::filesystem::path &path);
}
#endif
