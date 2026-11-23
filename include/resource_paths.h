#ifndef ENGINE_SIM_RESOURCE_PATHS_H
#define ENGINE_SIM_RESOURCE_PATHS_H

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine_sim {
class ResourceError : public std::runtime_error {
public:
    ResourceError(const std::filesystem::path &path, const std::string &reason)
        : std::runtime_error(path.empty() ? "resources: " + reason : "resources: " + path.u8string() + ": " + reason) {}
};

struct ResourceOptions {
    std::filesystem::path resources, model;
};

std::filesystem::path executablePath();
std::vector<std::string> processArguments(int argc, char **argv);
ResourceOptions parseResourceOptions(const std::vector<std::string> &arguments, bool allowOtherOptions = false);

struct ResourcePaths {
    std::filesystem::path root, assets, desktop, model;
    static std::filesystem::path defaultRoot(const std::filesystem::path &executable);
    static ResourcePaths resolve(const std::filesystem::path &executable, const ResourceOptions &options,
                                bool readLegacyConfiguration = false);
    void validateDesktop() const;
    void validateLegacyDesktop() const;
    std::filesystem::path scene() const { return assets / "assets.ysce"; }
    std::filesystem::path font() const { return desktop / "fonts/Silkscreen/slkscr.ttf"; }
};
}
#endif
