#include "resources.hpp"

#include "wave_reader.h"

#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <utility>

namespace engine_sim::legacy {
namespace {
namespace cfg = config_v1;
namespace fs = std::filesystem;

cfg::Diagnostic diagnostic(std::string_view code, const fs::path &path, std::string_view reason,
                           const ImpulseOrigin *origin = nullptr, std::size_t ordinal = 0) {
    cfg::Diagnostic result;
    result.code = code;
    result.stage = "resources";
    result.path = path.u8string();
    result.message = reason;
    if (origin) {
        result.pointer = "/components/impulses/" + std::to_string(ordinal) + "/path";
        result.component_id = origin->impulse.value;
        result.field = "path";
        result.message += " (declared by " + origin->declaring_unit.u8string() + ")";
    }
    return result;
}

bool portable_component(std::string_view name) {
    if (name.empty() || name == "." || name == ".." || name.back() == ' ' || name.back() == '.')
        return false;
    if (std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }) ||
        name.find_first_of("<>:\"/\\|?*") != std::string_view::npos)
        return false;

    std::string basename(name.substr(0, name.find('.')));
    for (auto &c : basename) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    if (basename == "CON" || basename == "PRN" || basename == "AUX" || basename == "NUL" ||
        basename == "CONIN$" || basename == "CONOUT$") return false;
    const auto prefix = std::string_view(basename).substr(0, 3);
    if (prefix == "COM" || prefix == "LPT") {
        const auto suffix = std::string_view(basename).substr(3);
        if ((suffix.size() == 1 && suffix[0] >= '1' && suffix[0] <= '9') ||
            suffix == "\xc2\xb9" || suffix == "\xc2\xb2" || suffix == "\xc2\xb3") return false;
    }
    return true;
}

std::string portable_path(const fs::path &relative) {
    if (relative.empty() || relative.is_absolute() || relative.has_root_path()) return {};
    for (const auto &component : relative)
        if (!portable_component(component.u8string())) return {};
    return relative.generic_u8string();
}

std::vector<unsigned char> read_wave(const fs::path &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto bytes = file.tellg();
    if (bytes < 0) throw WaveError(WaveFailure::Io, path, "cannot open impulse file");
    if (bytes < 12 || static_cast<std::uintmax_t>(bytes) > maxImpulseWaveBytes)
        throw WaveError(WaveFailure::Size, path, "WAVE size must be between 12 bytes and 64 MiB");
    std::vector<unsigned char> input(static_cast<std::size_t>(bytes));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(input.data()), static_cast<std::streamsize>(bytes)) ||
        file.peek() != std::char_traits<char>::eof() || file.bad())
        throw WaveError(WaveFailure::Io, path, "cannot read exactly one complete impulse file");
    return input;
}

struct Rebased {
    std::vector<PinnedWave> waves;
    std::vector<ResolvedImpulse> origins;
    std::vector<std::size_t> first_use;
};

using RebaseResult = std::variant<Rebased, cfg::Diagnostic>;

RebaseResult rebase(Snapshot &input, const fs::path &root) {
    Rebased result;
    std::unordered_map<fs::path, std::size_t> indices;
    auto &definitions = input.values.components.impulses;
    if (definitions.size() != input.impulses.size())
        return diagnostic("CFG_RESOURCE_PATH", root, "Selected impulse declarations are incomplete");
    for (std::size_t i = 0; i < input.impulses.size(); ++i) {
        auto &origin = input.impulses[i];
        if (origin.impulse != definitions[i].id || !origin.declaring_unit.is_absolute() ||
            origin.evaluated_filename.empty() ||
            std::any_of(origin.evaluated_filename.begin(), origin.evaluated_filename.end(),
                        [](unsigned char c) { return c < 0x20 || c == 0x7f; })) {
            return diagnostic("CFG_RESOURCE_PATH", origin.declaring_unit, "Invalid evaluated impulse declaration", &origin, i);
        }

        fs::path evaluated;
        try { evaluated = fs::u8path(origin.evaluated_filename); }
        catch (const fs::filesystem_error &error) {
            return diagnostic("CFG_RESOURCE_PATH", origin.declaring_unit, error.what(), &origin, i);
        }
        if (!evaluated.is_absolute() && evaluated.has_root_path())
            return diagnostic("CFG_RESOURCE_PATH", evaluated, "Impulse path has a drive-relative root", &origin, i);
        const auto opened = evaluated.is_absolute() ? evaluated : origin.declaring_unit.parent_path() / evaluated;
        std::error_code error;
        const auto actual = fs::canonical(opened, error);
        if (error || !fs::is_regular_file(actual, error) || error)
            return diagnostic("CFG_RESOURCE_PATH", opened, "Impulse source is not an existing regular file", &origin, i);
        auto relative = portable_path(actual.lexically_relative(root));
        if (relative.empty())
            return diagnostic("CFG_RESOURCE_PATH", actual, "Impulse source is outside the resource root or has an unsupported path", &origin, i);
        const auto [slot, new_path] = indices.try_emplace(actual, result.waves.size());
        definitions[i].path = relative;
        if (new_path) {
            result.waves.push_back({actual, std::move(relative), {}, 0});
            result.first_use.push_back(i);
        }
        result.origins.push_back({std::move(origin), slot->second});
    }
    return result;
}
}

ResolveResult resolve(Snapshot input, const fs::path &requested_root) {
    if (!requested_root.is_absolute())
        return diagnostic("CFG_RESOURCE_PATH", requested_root, "Expected an absolute resource root");

    std::error_code error;
    auto root = fs::canonical(requested_root, error);
    if (error || !fs::is_directory(root, error) || error)
        return diagnostic("CFG_RESOURCE_PATH", requested_root, "Resource root is not an existing directory");
    auto prepared = rebase(input, root);
    if (auto *failure = std::get_if<cfg::Diagnostic>(&prepared)) return std::move(*failure);
    auto &rebased = std::get<Rebased>(prepared);
    auto model = cfg::normalize(std::move(input.values));
    if (auto *failure = std::get_if<cfg::Diagnostic>(&model)) return std::move(*failure);

    for (std::size_t i = 0; i < rebased.waves.size(); ++i) {
        auto &wave = rebased.waves[i];
        try {
            wave.bytes = read_wave(wave.canonical_path);
            wave.samples = decodeImpulseWave(wave.bytes, wave.canonical_path).size();
        }
        catch (const WaveError &failure) {
            const auto code = failure.failure == WaveFailure::Io || failure.failure == WaveFailure::Size
                ? "CFG_RESOURCE_BYTES" : "CFG_RESOURCE_FORMAT";
            const auto source = rebased.first_use[i];
            return diagnostic(code, wave.canonical_path, failure.reason, &rebased.origins[source].origin, source);
        }
    }
    return ResolvedSnapshot{std::get<cfg::NormalizedDocument>(std::move(model)), std::move(root),
        std::move(rebased.origins), std::move(rebased.waves), std::move(input.ignored_inputs),
        std::move(input.consumed_scripts)};
}

}
