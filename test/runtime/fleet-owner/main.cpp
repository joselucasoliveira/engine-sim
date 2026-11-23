#include "layout.h"
#include "owner.hpp"
#include "engine_sim/runtime.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>

extern "C" int fleet_c_consumer(const char *entry, const char *root);

namespace {
struct Allocator {
    std::size_t attempts = 0;
    std::size_t failAt = 0;
};
thread_local Allocator *inject = nullptr;

void *allocate(std::size_t bytes) {
    if (inject && ++inject->attempts == inject->failAt) throw std::bad_alloc();
    if (auto *value = std::malloc(std::max<std::size_t>(bytes, 1))) return value;
    throw std::bad_alloc();
}
}
void *operator new(std::size_t bytes) { return allocate(bytes); }
void *operator new[](std::size_t bytes) { return allocate(bytes); }
void operator delete(void *storage) noexcept { std::free(storage); }
void operator delete[](void *storage) noexcept { std::free(storage); }
void operator delete(void *storage, std::size_t) noexcept { std::free(storage); }
void operator delete[](void *storage, std::size_t) noexcept { std::free(storage); }

namespace {
namespace fs = std::filesystem;
using Fleet = std::unique_ptr<es_rt_fleet, decltype(&es_rt_destroy)>;
constexpr es_rt_token canary{91, 92, 93, 94};

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string read(const fs::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    require(size > 0 && size < 262144, "Could not read fixture bytes");
    std::string text(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    require(static_cast<bool>(input.read(text.data(), static_cast<std::streamsize>(size))), "Incomplete fixture");
    return text;
}

Fleet owner(std::uint64_t epoch, unsigned rate = 48000, unsigned lanes = 2) {
    const es_rt_config setup{sizeof(setup), ES_RT_V1, rate, ES_RT_BALANCED, 256, lanes, ES_RT_EXPERIMENTAL, 0, epoch};
    es_rt_fleet *target = nullptr;
    require(es_rt_create(&setup, &target, nullptr) == ES_RT_OK, "Experimental fleet creation failed");
    return Fleet(target, es_rt_destroy);
}

void layout(const std::string &entry, const std::string &root) {
    const auto runtime_abi = es_api_version();
    const auto runtime_capabilities = es_capabilities();
    require(fleet_c_consumer(entry.c_str(), root.c_str()) == 0, "Strict C11 fleet consumer failed");
    require(es_rt_api_version() == ES_RT_V1 && es_api_version() == runtime_abi && runtime_abi == ES_ABI_V2 &&
        es_capabilities() == runtime_capabilities && runtime_capabilities != 0, "Fleet altered runtime v2 negotiation");
    for (unsigned lanes : {1u, 8u}) {
        auto fleet = owner(99 + lanes, 44100, lanes);
        require(fleet->cellCount == lanes + 3, "Wrong fixed count for logical lane boundary");
        for (std::size_t i = 0; i < fleet->cellCount; ++i)
            require(fleet->cells[i].state.is_lock_free() && fleet->cells[i].generation.is_lock_free(),
                "An actual owner-cell atomic is not lock-free");
    }
    std::cout << "All published v1 record sizes and offsets match in strict C11 and C++17.\n";
}

void compare_first(engine_sim::fleet::Voice &candidate, const std::string &entry, const std::string &root,
                   const es_rt_voice_config &settings, unsigned rate) {
    es_config config{sizeof(config), ES_ABI_V2, rate, settings.mode == ES_RT_FREE ? ES_FREE : ES_DRIVEN,
                     settings.seed, settings.simulation_hz, 0, settings.starter_torque_nm};
    es_runtime *handle = nullptr;
    require(es_create(&config, &handle, nullptr) == ES_OK, "Raw voice could not create");
    std::unique_ptr<es_runtime, decltype(&es_destroy)> raw(handle, es_destroy);
    require(es_load_json_file(raw.get(), entry.c_str(), root.c_str(), nullptr, nullptr) == ES_OK,
            "Raw voice could not load independent file");
    const auto &vector = settings.initial;
    const es_controls controls{sizeof(controls), ES_ABI_V2, vector.ignition, vector.starter, vector.demand, vector.load_nm, vector.driven_rpm};
    require(es_set_controls(raw.get(), &controls, nullptr) == ES_OK, "Raw initial vector failed");
    const auto &initial = candidate.runtime.telemetry();
    require(initial.frames == 0 && initial.ticks == 0 && initial.native_frames == 0 && initial.requested_load_nm == vector.load_nm,
        "Prepared cell advanced or did not retain initial control request");
    std::array<float, 256> a{}, b{};
    std::uint32_t wroteA = 0, wroteB = 0;
    require(candidate.runtime.renderChecked(a.data(), a.size(), wroteA) == engine_sim::ProcessingStatus::ok && wroteA == a.size(),
        "Detached candidate did not produce the first block");
    require(es_render(raw.get(), b.size(), b.data(), b.size(), &wroteB, nullptr) == ES_OK && wroteB == b.size(),
        "Raw voice did not produce the first block");
    require(std::memcmp(a.data(), b.data(), sizeof(a)) == 0, "Candidate first PCM differs from raw v2");
    es_telemetry expected{};
    expected.size = sizeof(expected); expected.abi = ES_ABI_V2;
    require(es_get_telemetry(raw.get(), &expected, nullptr) == ES_OK &&
        std::memcmp(&candidate.runtime.telemetry(), &expected, sizeof(expected)) == 0,
        "Candidate first-frame telemetry differs from raw v2");
}

void cells(const std::string &entry, const std::string &root, const std::string &content) {
    auto fleet = owner(100), other = owner(101);
    const es_rt_voice_config free{sizeof(es_rt_voice_config), ES_RT_V1, ES_RT_FREE, 0, ES_RT_RAW, 0, 403, 0,
                                  {.25, .375, 0, 1, 1}};
    const es_rt_voice_config driven{sizeof(es_rt_voice_config), ES_RT_V1, ES_RT_DRIVEN, 14321, ES_RT_RAW, 0, 406, 0,
                                    {.55, .25, 0, 1, 0}};
    es_rt_token file = canary, bytes = canary, out = canary;
    require(es_rt_prepare_file(fleet.get(), &free, entry.c_str(), root.c_str(), &file, nullptr) == ES_RT_OK,
            "Private file candidate failed");
    auto source = content;
    const auto prepare_bytes = [&](es_rt_token &token) {
        return es_rt_prepare_bytes(fleet.get(), &driven, reinterpret_cast<const uint8_t *>(source.data()),
            source.size(), root.c_str(), &token, nullptr);
    };
    Allocator total;
    inject = &total;
    const auto prepared = prepare_bytes(bytes);
    inject = nullptr;
    require(prepared == ES_RT_OK && total.attempts > 20, "Byte candidate or allocation witness failed");
    source.assign(source.size(), '?');
    require(file.cell != bytes.cell && fleet->prepared == 2 && es_rt_discard(other.get(), file) == ES_RT_STALE,
            "Candidate token crossed fleet epoch or cells overlapped");
    require(es_rt_prepare_file(fleet.get(), &free, entry.c_str(), root.c_str(), &out, nullptr) == ES_RT_FULL &&
        std::memcmp(&out, &canary, sizeof(out)) == 0, "Third candidate changed caller storage or was accepted");
    auto &second = *fleet->cells[bytes.cell].voice;
    require(second.capacity.assembled.rows == second.capacity.predicted.rows &&
        second.capacity.assembled.generators == second.capacity.predicted.generators &&
        second.capacity.retained_bytes <= engine_sim::assembly::fleet_cell_limit &&
        second.capacity.document_bytes == content.size() && second.capacity.unique_wave_bytes > 0,
        "Fleet byte candidate did not retain its bounded source and final mode graph");
    compare_first(second, entry, root, driven, 48000);
    require(es_rt_discard(fleet.get(), bytes) == ES_RT_OK && es_rt_discard(fleet.get(), bytes) == ES_RT_STALE,
        "Discard failed to expire generation");
    require(prepare_bytes(out) == ES_RT_MODEL && fleet->prepared == 1, "Invalid bytes consumed candidate credit");
    const auto missing = (fs::u8path(root) / "absent.json").u8string();
    require(es_rt_prepare_file(fleet.get(), &driven, missing.c_str(), root.c_str(), &out, nullptr) == ES_RT_RESOURCE &&
        fleet->prepared == 1, "Missing resource consumed candidate credit");
    require(es_rt_prepare_bytes(fleet.get(), &driven, reinterpret_cast<const uint8_t *>("x"), UINT64_MAX,
        root.c_str(), &out, nullptr) == ES_RT_CAPACITY, "Unaddressable byte span was consumed");
    require(std::memcmp(&out, &canary, sizeof(out)) == 0, "A rejected candidate overwrote its output token");
    source = content;
    for (const auto point : {std::size_t{1}, total.attempts}) {
        Allocator failure{0, point};
        inject = &failure;
        const auto status = prepare_bytes(out);
        inject = nullptr;
        require(status == ES_RT_MEMORY && failure.attempts == point && fleet->prepared == 1 &&
            std::memcmp(&out, &canary, sizeof(out)) == 0, "Injected allocation failure changed the owner or destination");
    }
    compare_first(*fleet->cells[file.cell].voice, entry, root, free, 48000);
    require(prepare_bytes(out) == ES_RT_OK && out.cell == bytes.cell && out.generation > bytes.generation,
        "A failed preparation stranded a credit or reused a token");
    require(es_rt_discard(fleet.get(), bytes) == ES_RT_STALE && es_rt_discard(fleet.get(), file) == ES_RT_OK,
        "Expired generation gained authority after reuse");
    std::cout << "Two independent candidate cells preserved PCM/telemetry. OOM points 1 and " << total.attempts
              << " were rejected transactionally.\n";
}
}

int main(int argc, char **argv) try {
    require(argc == 3, "Expected suite and fixture");
    const auto model = fs::absolute(argv[2]);
    const auto entry = model.u8string(), root = model.parent_path().u8string();
    if (std::string_view(argv[1]) == "layout") layout(entry, root);
    else cells(entry, root, read(model));
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
