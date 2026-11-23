#include "../include/resource_paths.h"

#include <fstream>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace engine_sim {
namespace fs = std::filesystem;
namespace {
fs::path absolutePath(const fs::path &path) {
    std::error_code error;
    auto absolute = fs::absolute(path, error);
    if (error) throw ResourceError(path, "cannot resolve absolute path");
    auto result = fs::weakly_canonical(absolute, error);
    if (error) throw ResourceError(path, "cannot resolve path components");
    return result;
}
void requireFile(const fs::path &path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error)) throw ResourceError(path, "required file is missing or unreadable");
}
void requireDirectory(const fs::path &path) {
    std::error_code error;
    if (!fs::is_directory(path, error)) throw ResourceError(path, "required directory is missing or unreadable");
}
fs::path legacyPath(const fs::path &module, const std::string &text) {
    // Historical packages used these two slash-prefixed module-relative names.
    if (text == "/delta" || text == "\\delta") return module / "delta";
    if (text == "/assets" || text == "\\assets") return module / "assets";
    const auto path = fs::u8path(text);
    return path.is_absolute() ? path : module / path;
}
}

fs::path executablePath() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) throw ResourceError({}, "cannot query executable path");
        if (length < buffer.size()) return absolutePath(fs::path(std::wstring(buffer.data(), length)));
        if (buffer.size() >= 32768) throw ResourceError({}, "executable path exceeds Windows limit");
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) throw ResourceError({}, "cannot query executable path");
    return absolutePath(fs::path(buffer.data()));
#elif defined(__linux__)
    std::error_code error;
    const auto path = fs::read_symlink("/proc/self/exe", error);
    if (error) throw ResourceError("/proc/self/exe", "cannot query executable path");
    return absolutePath(path);
#else
    throw ResourceError({}, "executable path query is unsupported on this host");
#endif
}

std::vector<std::string> processArguments(int argc, char **argv) {
    std::vector<std::string> arguments;
#ifdef _WIN32
    int count = 0;
    auto **wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) throw ResourceError({}, "cannot decode process arguments");
    struct Release { wchar_t **value; ~Release() { LocalFree(value); } } release{wide};
    for (int i = 0; i < count; ++i) arguments.push_back(fs::path(wide[i]).u8string());
    (void)argc; (void)argv;
#else
    for (int i = 0; i < argc; ++i) arguments.emplace_back(argv[i]);
#endif
    return arguments;
}

ResourceOptions parseResourceOptions(const std::vector<std::string> &arguments, bool allowOtherOptions) {
    ResourceOptions options;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const auto &option = arguments[i];
        if (option == "--script" || option.rfind("--script=", 0) == 0)
            throw ResourceError({}, "--script was removed; convert the model with engine-sim-convert and pass --model");
        if (option == "--resources" || option == "--model") {
            if (++i == arguments.size() || arguments[i].empty()) throw ResourceError({}, option + " requires a path");
            auto &target = option == "--resources" ? options.resources : options.model;
            if (!target.empty()) throw ResourceError({}, "duplicate " + option);
            target = fs::u8path(arguments[i]);
        }
        else if (!allowOtherOptions) throw ResourceError({}, "unknown option " + option);
    }
    return options;
}

fs::path ResourcePaths::defaultRoot(const fs::path &executable) {
    const auto directory = absolutePath(executable).parent_path();
    if (directory.filename() == "MacOS" && directory.parent_path().filename() == "Contents") {
        return directory.parent_path() / "Resources";
    }
    return directory.parent_path() / "share/engine-sim";
}

ResourcePaths ResourcePaths::resolve(const fs::path &executable, const ResourceOptions &options,
                                     bool readLegacyConfiguration) {
    ResourcePaths paths;
    const auto directory = absolutePath(executable).parent_path();
    paths.root = options.resources.empty() ? defaultRoot(executable) : absolutePath(options.resources);
    paths.assets = paths.root / "assets";
    paths.desktop = paths.root / "delta";
    if (!options.resources.empty() && !fs::is_directory(paths.desktop)) {
        const auto development = paths.root / "dependencies/submodules/delta-studio/engines/basic";
        if (fs::is_directory(development)) paths.desktop = development;
    }
    const auto configuration = directory / "delta.conf";
    if (readLegacyConfiguration && options.resources.empty() && fs::exists(configuration)) {
        std::ifstream file(configuration);
        std::string desktop, assets, extra;
        if (!std::getline(file, desktop) || !std::getline(file, assets)) throw ResourceError(configuration, "expected two UTF-8 paths");
        if (!desktop.empty() && desktop.back() == '\r') desktop.pop_back();
        if (!assets.empty() && assets.back() == '\r') assets.pop_back();
        if (desktop.compare(0, 3, "\xEF\xBB\xBF") == 0) desktop.erase(0, 3);
        if (desktop.empty() || assets.empty()) throw ResourceError(configuration, "configuration paths cannot be empty");
        while (std::getline(file, extra)) if (!extra.empty() && extra != "\r") throw ResourceError(configuration, "expected exactly two paths");
        paths.desktop = absolutePath(legacyPath(directory, desktop));
        paths.assets = absolutePath(legacyPath(directory, assets));
        paths.root = paths.assets.parent_path();
    }
    paths.model = options.model.empty() ? paths.assets / "main.json" : absolutePath(options.model);
    return paths;
}

void ResourcePaths::validateDesktop() const {
    requireDirectory(assets);
    requireFile(model);
    requireFile(scene());
}
void ResourcePaths::validateLegacyDesktop() const {
    validateDesktop(); requireFile(font());
    for (const auto *name : {"delta_console_shader.fx", "delta_engine_shader.fx", "delta_saq_shader.fx"}) {
        requireFile(desktop / "shaders/hlsl" / name);
    }
    if ((assets / "assets").u8string().size() + 6 > 512) throw ResourceError(scene(), "path exceeds legacy scene loader limit");
}
}
