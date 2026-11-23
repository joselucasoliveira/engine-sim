#include "json_runtime.hpp"
#include "runtime.hpp"
#include "simulation_fault.h"

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace engine_sim {
struct RuntimeTestAccess {
    static void faultNextTick(es_runtime &runtime) {
        runtime.implementation.m_session->pull->setTickObserver([](const Simulator &) {
            throw SimulationFault("injected tick failure");
        });
    }
};
}

namespace {
namespace json = engine_sim::json_runtime;
namespace cfg = engine_sim::config_v1;
namespace fs = std::filesystem;

using Handle = std::unique_ptr<es_runtime, decltype(&es_destroy)>;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void checked(es_status status, const es_error &error) {
    if (status != ES_OK) throw std::runtime_error(error.message);
}

struct Fixture {
    fs::path directory;
    fs::path entry;
    std::string document;
    std::string impulse;

    Fixture(const fs::path &config, const fs::path &response)
        : document(read(config)), impulse(read(response)) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = fs::temp_directory_path() / ("engine-sim-json-runtime-" + std::to_string(stamp));
        entry = directory / "model.json";
        require(fs::create_directory(directory), "Unable to create isolated runtime fixture");
        try {
            write(entry, document);
            write(directory / "impulse.wav", impulse);
        } catch (...) {
            std::error_code ignored;
            fs::remove_all(directory, ignored);
            throw;
        }
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
    }

    static std::string read(const fs::path &path) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        const auto length = in.tellg();
        require(length > 0 && length < 16 * 1024 * 1024, "Unable to open runtime fixture");
        std::string result(static_cast<std::size_t>(length), '\0');
        in.seekg(0);
        require(static_cast<bool>(in.read(result.data(), static_cast<std::streamsize>(length))), "Incomplete runtime fixture");
        return result;
    }
    static void write(const fs::path &path, std::string_view bytes) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        require(out.good(), "Unable to write runtime fixture");
    }
};

Handle create(std::uint32_t rate, std::uint32_t mode) {
    es_error error{};
    es_config config{};
    checked(es_config_init(ES_ABI_V1, sizeof(config), &config, &error), error);
    config.output_hz = rate;
    config.mode = mode;
    config.seed = 403;
    config.simulation_hz = 14321;
    es_runtime *runtime = nullptr;
    checked(es_create(&config, &runtime, &error), error);
    return Handle(runtime, es_destroy);
}

es_telemetry telemetry(es_runtime *runtime) {
    es_error error{};
    es_telemetry result{};
    result.size = sizeof(result);
    result.abi = ES_ABI_V1;
    checked(es_get_telemetry(runtime, &result, &error), error);
    return result;
}

void same(es_runtime *a, es_runtime *b) {
    const auto left = telemetry(a);
    const auto right = telemetry(b);
    require(std::memcmp(&left, &right, sizeof(left)) == 0, "Rejected operation changed telemetry or sequencer state");
}

void controls(es_runtime *runtime, bool driven, unsigned phase) {
    es_controls next{sizeof(es_controls), ES_ABI_V1, 1, driven || phase > 1 ? 0u : 1u,
                     0.25 + (phase % 3) * 0.15, 0.5 + (phase % 3) * 0.125, driven ? 450.0 + phase * 50 : 0.0};
    es_error error{};
    checked(es_set_controls(runtime, &next, &error), error);
}

std::vector<float> render(es_runtime *runtime, std::uint32_t frames) {
    std::vector<float> result(frames);
    es_error error{};
    std::uint32_t written = 99;
    checked(es_render(runtime, frames, frames == 0 ? nullptr : result.data(), frames, &written, &error), error);
    require(written == frames, "Successful render returned an incomplete prefix");
    return result;
}

void advance(es_runtime *a, es_runtime *b, unsigned phase) {
    const auto mode = telemetry(a).mode;
    controls(a, mode == ES_DRIVEN, phase);
    controls(b, mode == ES_DRIVEN, phase);
    same(a, b);
    const auto contiguous = render(a, 793);
    std::vector<float> irregular;
    for (const std::uint32_t count : {0u, 7u, 251u, 31u, 1u, 503u}) {
        const auto chunk = render(b, count);
        irregular.insert(irregular.end(), chunk.begin(), chunk.end());
    }
    require(contiguous.size() == irregular.size() &&
            std::memcmp(contiguous.data(), irregular.data(), irregular.size() * sizeof(float)) == 0,
            "Irregular PCM after failure differs from the untouched session");
    same(a, b);
}

void expect(es_status found, es_status status, const cfg::Diagnostic &detail, std::string_view code) {
    if (found != status || detail.code != code)
        throw std::runtime_error("Unexpected result " + std::to_string(found) + " " + detail.code + " instead of " +
                                 std::to_string(status) + " " + std::string(code));
}

void load_pair(es_runtime *file, es_runtime *owned, const Fixture &fixture) {
    es_error error{};
    checked(json::load_file(file, fixture.entry, fixture.directory, nullptr, &error), error);
    auto caller = fixture.document;
    checked(json::load_bytes(owned, caller.data(), caller.size(), fixture.directory, nullptr, &error), error);
    caller.assign(caller.size(), '!');
    same(file, owned);
}

void transactional(const Fixture &fixture) {
    for (const std::uint32_t mode : {ES_FREE, ES_DRIVEN}) {
        for (const std::uint32_t rate : {44100u, 48000u}) {
            auto a = create(rate, mode), b = create(rate, mode);
            cfg::Diagnostic detail;
            es_error error{};
            expect(json::reset(a.get(), &detail), ES_STATE, detail, "API_STATE");
            expect(json::load_bytes(a.get(), nullptr, 1, fixture.directory, &detail), ES_INVALID, detail, "API_INVALID");
            expect(json::load_bytes(a.get(), nullptr, 0, fixture.directory, &detail),
                   json::configuration_error, detail, "CFG_SYNTAX");
            expect(json::load_bytes(a.get(), "x", std::numeric_limits<std::uint64_t>::max(), fixture.directory, &detail),
                   ES_CAPACITY, detail, "API_CAPACITY");
            expect(json::load_bytes(a.get(), "x", 16 * 1024 * 1024 + 1, fixture.directory, &detail),
                   json::configuration_error, detail, "CFG_LIMIT");
            expect(json::load_file(a.get(), "relative.json", fixture.directory, &detail), ES_INVALID, detail, "API_INVALID");
            require(telemetry(a.get()).state == ES_EMPTY, "Preflight advanced an empty handle");

            load_pair(a.get(), b.get(), fixture);
            advance(a.get(), b.get(), 0);
            struct Invalid { std::string_view bytes; es_status status; const char *code; };
            const Invalid failures[]{
                {"{", json::configuration_error, "CFG_SYNTAX"},
                {"{\"format_version\":2}", ES_VERSION, "CFG_VERSION"},
                {"{\"format_version\":1}", json::configuration_error, "CFG_SHAPE"},
                {"{\"format_version\":1,\"format_version\":1}", json::configuration_error, "CFG_DUPLICATE_KEY"},
            };
            controls(a.get(), mode == ES_DRIVEN, 1);
            controls(b.get(), mode == ES_DRIVEN, 1);
            for (const auto &fault : failures) {
                expect(json::load_bytes(a.get(), fault.bytes.data(), fault.bytes.size(), fixture.directory, &detail),
                       fault.status, detail, fault.code);
                same(a.get(), b.get());
            }

            auto invalid_graph = fixture.document;
            const auto target = invalid_graph.find("\"head\": \"top\"");
            require(target != std::string::npos, "Fixture no longer has the intended head reference");
            invalid_graph.replace(target, std::strlen("\"head\": \"top\""), "\"head\": \"absent\"");
            expect(json::load_bytes(a.get(), invalid_graph.data(), invalid_graph.size(), fixture.directory, &detail),
                   ES_MODEL, detail, "CFG_REF");
            const auto absent = fixture.directory / "missing";
            expect(json::load_file(a.get(), fixture.entry, absent, &detail), ES_RESOURCE, detail, "CFG_RESOURCE_PATH");

            for (const auto phase : {json::Phase::document, json::Phase::normalized,
                                      json::Phase::prepared, json::Phase::snapshot}) {
                const auto stop = [&](json::Phase observed) { if (observed == phase) throw std::bad_alloc(); };
                require(json::load_file(a.get(), fixture.entry, fixture.directory, &detail, &error, stop) == ES_NO_MEMORY,
                        "Allocation failure did not translate to its bounded status");
                require(std::string_view(error.message).find("CFG_MEMORY") == 0, "Missing bounded out-of-memory diagnostic");
                same(a.get(), b.get());
            }
            const auto invalid_snapshot = [](json::Phase phase) {
                if (phase == json::Phase::snapshot) throw engine_sim::SimulationFault("injected snapshot failure");
            };
            expect(json::load_file(a.get(), fixture.entry, fixture.directory, &detail, &error, invalid_snapshot),
                   ES_MODEL, detail, "CFG_INITIALIZE");
            require(detail.stage == "initialize", "Snapshot failure missed the initialization diagnostic stage");
            advance(a.get(), b.get(), 1);

            engine_sim::RuntimeTestAccess::faultNextTick(*a);
            std::array<float,64> pcm{};
            std::uint32_t written = 0;
            require(es_render(a.get(), pcm.size(), pcm.data(), pcm.size(), &written, &error) == ES_RENDER_FAULT,
                    "Fixture did not produce the typed render fault");
            const auto faulted = telemetry(a.get());
            require(faulted.state == ES_FAULTED, "Render failure was not committed as FAULTED");
            const auto after_snapshot = [](json::Phase phase) { if (phase == json::Phase::snapshot) throw std::bad_alloc(); };
            require(json::reset(a.get(), &detail, &error, after_snapshot) == ES_NO_MEMORY,
                    "Faulted reset allocation test missed activation");
            const auto unchanged = telemetry(a.get());
            require(std::memcmp(&faulted, &unchanged, sizeof(faulted)) == 0, "Failed reset changed FAULTED telemetry");
            checked(json::reset(a.get(), &detail, &error), error);
            checked(json::reset(b.get(), &detail, &error), error);
            same(a.get(), b.get());
            advance(a.get(), b.get(), 2);
        }
    }
}

void identity(const Fixture &fixture) {
    auto a = create(48000, ES_FREE), b = create(48000, ES_FREE);
    auto file = a.get();
    auto owned = b.get();
    load_pair(file, owned, fixture);
    advance(file, owned, 0);
    es_error error{};
    cfg::Diagnostic detail;
    const auto bytes = fixture.document + "\n ";
    Fixture::write(fixture.entry, bytes);
    expect(json::reset(file, &detail), ES_RESOURCE, detail, "CFG_RESOURCE_BYTES");
    same(file, owned);
    Fixture::write(fixture.entry, std::string(fixture.document.size(), '!'));
    expect(json::reset(file, &detail), ES_RESOURCE, detail, "CFG_RESOURCE_BYTES");
    same(file, owned);
    Fixture::write(fixture.entry, fixture.document);

    const auto response = fixture.directory / "impulse.wav";
    auto changed = fixture.impulse;
    changed.back() ^= 0x01;
    Fixture::write(response, changed);
    for (auto *runtime : {file, owned}) {
        expect(json::reset(runtime, &detail), ES_RESOURCE, detail, "CFG_RESOURCE_BYTES");
        same(file, owned);
    }
    fs::rename(response, fixture.directory / "moved.wav");
    expect(json::reset(owned, &detail), ES_RESOURCE, detail, "CFG_RESOURCE_PATH");
    fs::remove(fixture.directory / "moved.wav");
    Fixture::write(response, fixture.impulse);
    advance(file, owned, 1);

    // The recorded input set has no legacy path to read or pin.
    Fixture::write(fixture.directory / "legacy-import.mr", "broken unrelated script");
    checked(json::reset(file, &detail, &error), error);
    fs::remove(fixture.directory / "legacy-import.mr");
    fs::remove(fixture.entry);
    checked(json::reset(owned, &detail, &error), error);
    same(file, owned);
    advance(file, owned, 2);
    Fixture::write(fixture.entry, bytes);
    checked(json::load_file(file, fixture.entry, fixture.directory, &detail, &error), error);
    require(telemetry(file).frames == 0, "Fresh file load did not accept the changed valid document");

    const auto directory_a = fixture.directory / "first";
    const auto directory_b = fixture.directory / "second";
    fs::create_directory(directory_a);
    fs::create_directory(directory_b);
    Fixture::write(directory_a / "impulse.wav", fixture.impulse);
    Fixture::write(directory_b / "impulse.wav", fixture.impulse);
    const auto alias = fixture.directory / "alias";
    std::error_code link_error;
    fs::create_directory_symlink(directory_a, alias, link_error);
#ifdef _WIN32
    if (link_error) std::cout << "Native directory symlink unavailable: " << link_error.message() << '\n';
#else
    require(!link_error, "Cannot create directory symlink fixture");
#endif
    if (!link_error) {
        checked(json::load_file(file, fixture.entry, alias, &detail, &error), error);
        fs::remove(alias);
        fs::create_directory_symlink(directory_b, alias);
        expect(json::reset(file, &detail), ES_RESOURCE, detail, "CFG_RESOURCE_BYTES");
        const auto entry_alias = fixture.directory / "alias.json";
        fs::create_symlink(fixture.entry, entry_alias);
        checked(json::load_file(file, entry_alias, fixture.directory, &detail, &error), error);
        Fixture::write(directory_a / "copy.json", bytes);
        fs::remove(entry_alias);
        fs::create_symlink(directory_a / "copy.json", entry_alias);
        expect(json::reset(file, &detail), ES_RESOURCE, detail, "CFG_RESOURCE_BYTES");
    }
    checked(es_unload(file, &error), error);
    expect(json::reset(file, &detail), ES_STATE, detail, "API_STATE");
}
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    try {
        const Fixture fixture(fs::u8path(argv[2]), fs::u8path(argv[3]));
        const std::string_view mode = argv[1];
        if (mode == "transactional") transactional(fixture);
        else if (mode == "identity") identity(fixture);
        else return 2;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
