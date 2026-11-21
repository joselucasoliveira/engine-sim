#include "../include/compiled_scene.h"
#include "../include/resource_paths.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>

namespace engine_sim {
namespace {
class SceneInput {
public:
    explicit SceneInput(const std::filesystem::path &path) : path(path) {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size < 4 || size > 8 * 1024 * 1024) fail("missing scene or unsupported file size");
        bytes.resize(static_cast<std::size_t>(size));
        std::ifstream file(path, std::ios::binary);
        if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size))) fail("truncated scene file");
    }
    [[noreturn]] void fail(const char *reason) const { throw ResourceError(path, reason); }
    std::uint32_t unsignedValue(unsigned size = 4) {
        if (size > bytes.size() - offset) fail("truncated scene field");
        std::uint32_t result = 0;
        for (unsigned i = 0; i < size; ++i) result |= static_cast<std::uint32_t>(bytes[offset++]) << (i * 8);
        return result;
    }
    std::int32_t integer() {
        const auto bits = unsignedValue();
        std::int32_t value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    float number() {
        static_assert(sizeof(float) == 4);
        const auto bits = unsignedValue();
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value)) fail("nonfinite scene coordinate");
        return value;
    }
    std::string name() {
        if (bytes.size() - offset < 64) fail("truncated scene name");
        const auto first = bytes.begin() + offset;
        const auto end = std::find(first, first + 64, 0);
        if (end == first + 64) fail("scene name is not terminated");
        const std::string result(first, end);
        offset += 64;
        return result;
    }
    bool done() const { return offset == bytes.size(); }
private:
    std::filesystem::path path;
    std::vector<unsigned char> bytes;
    std::size_t offset = 0;
};
}

CompiledScene readCompiledScene(const std::filesystem::path &path) {
    SceneInput input(path);
    const auto count = input.unsignedValue();
    if (count == 0 || count > 1024) input.fail("invalid scene object count");
    CompiledScene scene;
    scene.meshes.reserve(count);
    std::set<std::string> names;
    std::size_t vertexBytes = 0, indexBytes = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        SceneMesh mesh;
        mesh.name = input.name(); mesh.material = input.name();
        if (mesh.name.empty() || !names.insert(mesh.name).second) input.fail("missing or duplicate scene model name");
        input.integer(); // ModelIndex is unused by the static geometry loader.
        mesh.parent = input.integer();
        input.integer(); input.integer(); // Instance and skeleton indices are unused for this layout.
        if (input.integer() != 0) input.fail("unsupported scene object type; expected static geometry");
        if (mesh.parent < -1 || mesh.parent >= static_cast<std::int32_t>(count)) input.fail("scene parent is out of range");
        for (auto &v : mesh.position) v = input.number();
        for (auto &v : mesh.eulerDegrees) v = input.number();
        for (auto &v : mesh.orientation) v = input.number();
        for (auto &v : mesh.scale) v = input.number();
        for (int j = 0; j < 6; ++j) input.number(); // Bounds are finite even though drawing uses the vertices.
        const auto uvChannels = input.unsignedValue();
        const auto vertices = input.unsignedValue(), faces = input.unsignedValue();
        const auto bones = input.unsignedValue();
        input.unsignedValue(); // Maximum bone weights is unused when no bones exist.
        const auto flags = input.unsignedValue(), size = input.unsignedValue();
        if (uvChannels != 1 || bones != 0 || flags != 10) input.fail("unsupported scene vertex layout");
        if (vertices == 0 || vertices > 65535 || faces == 0 || faces > 1024 * 1024 / 6
            || size != static_cast<std::uint64_t>(vertices) * 40) input.fail("invalid scene geometry sizes");
        vertexBytes += size;
        indexBytes += static_cast<std::size_t>(faces) * 6;
        if (vertexBytes > 4 * 1024 * 1024 || indexBytes > 1024 * 1024) input.fail("scene exceeds legacy GPU buffer capacities");
        mesh.vertices.resize(vertices);
        for (auto &vertex : mesh.vertices) for (auto &value : vertex) value = input.number();
        mesh.indices.reserve(static_cast<std::size_t>(faces) * 3);
        for (std::size_t j = 0; j < static_cast<std::size_t>(faces) * 3; ++j) {
            const auto index = input.unsignedValue(2);
            if (index >= vertices) input.fail("scene index exceeds its model vertices");
            mesh.indices.push_back(static_cast<std::uint16_t>(index));
        }
        scene.meshes.push_back(std::move(mesh));
    }
    if (!input.done()) input.fail("trailing scene data");
    for (std::size_t i = 0; i < scene.meshes.size(); ++i) {
        std::vector<bool> visited(scene.meshes.size());
        for (auto parent = static_cast<std::int32_t>(i); parent >= 0; parent = scene.meshes[parent].parent) {
            if (visited[parent]) input.fail("scene parent graph contains a cycle");
            visited[parent] = true;
        }
    }
    for (const auto *name : {"Crankshaft", "CrankSnout", "CrankSnoutThreads", "ConnectingRod", "Piston", "CylinderHead", "Valve", "Logo"}) {
        if (names.count(name) == 0) input.fail("required engine display model is missing");
    }
    return scene;
}
}
