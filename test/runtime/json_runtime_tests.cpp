#include "json_runtime.hpp"
#include "runtime.hpp"
#include "simulation_fault.h"

#include <algorithm>
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
    checked(es_config_init(ES_ABI_V2, sizeof(config), &config, &error), error);
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
    result.abi = ES_ABI_V2;
    checked(es_get_telemetry(runtime, &result, &error), error);
    return result;
}

void same(es_runtime *a, es_runtime *b) {
    const auto left = telemetry(a);
    const auto right = telemetry(b);
    require(std::memcmp(&left, &right, sizeof(left)) == 0, "Rejected operation changed telemetry or sequencer state");
}

void controls(es_runtime *runtime, bool driven, unsigned phase) {
    es_controls next{sizeof(es_controls), ES_ABI_V2, 1, driven || phase > 1 ? 0u : 1u,
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
                   ES_CONFIG, detail, "CFG_SYNTAX");
            expect(json::load_bytes(a.get(), "x", std::numeric_limits<std::uint64_t>::max(), fixture.directory, &detail),
                   ES_CAPACITY, detail, "API_CAPACITY");
            expect(json::load_bytes(a.get(), "x", 16 * 1024 * 1024 + 1, fixture.directory, &detail),
                   ES_CONFIG, detail, "CFG_LIMIT");
            expect(json::load_file(a.get(), "relative.json", fixture.directory, &detail), ES_INVALID, detail, "API_INVALID");
            require(telemetry(a.get()).state == ES_EMPTY, "Preflight advanced an empty handle");

            load_pair(a.get(), b.get(), fixture);
            advance(a.get(), b.get(), 0);
            struct Invalid { std::string_view bytes; es_status status; const char *code; };
            const Invalid failures[]{
                {"{", ES_CONFIG, "CFG_SYNTAX"},
                {"{\"format_version\":3}", ES_VERSION, "CFG_VERSION"},
                {"{\"format_version\":1}", ES_CONFIG, "CFG_SHAPE"},
                {"{\"format_version\":1,\"format_version\":1}", ES_CONFIG, "CFG_DUPLICATE_KEY"},
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

void public_abi(const Fixture &fixture) {
    require(es_api_version() == ES_ABI_V2 && es_capabilities() == 1023, "Unexpected runtime version or capability set");
    auto runtime = create(44100, ES_FREE);
    struct Extended {
        es_diagnostic detail;
        std::array<unsigned char,16> tail;
    };
    Extended out;
    std::memset(&out, 0xa5, sizeof(out));
    auto &detail = out.detail;
    const auto root = fixture.directory.u8string();
    const auto entry = fixture.entry.u8string();
    es_error error{};
    detail.size = sizeof(out);
    detail.abi = 0x00010000;
    auto before = out;
    require(es_load_json_file(runtime.get(), entry.c_str(), root.c_str(), &detail, &error) == ES_VERSION,
            "A v1 record must be rejected");
    require(std::memcmp(&before, &out, sizeof(out)) == 0, "Rejected ABI wrote diagnostic bytes");
    detail.abi = ES_ABI_V2;
    detail.size = sizeof(detail) - 1;
    before = out;
    require(es_reset_detailed(runtime.get(), &detail, &error) == ES_CAPACITY &&
            std::memcmp(&before, &out, sizeof(out)) == 0,
            "Rejected diagnostic capacity touched caller storage");

    detail.size = sizeof(out);
    const auto check = [&](es_status status, es_status expected, std::uint32_t stage, std::string_view code) {
        require(status == expected && detail.stage == stage && detail.code == code,
                "Unexpected caller-owned diagnostic");
        require(out.tail == before.tail && detail.abi == ES_ABI_V2 && detail.size == sizeof(out),
                "A C diagnostic modified its prefix metadata or the caller tail");
    };
    check(es_reset_detailed(runtime.get(), &detail, &error), ES_STATE, ES_DIAG_API, "API_STATE");
    const char badPath[] = {'/', static_cast<char>(0xc0), static_cast<char>(0x80), 0};
    check(es_load_json_file(runtime.get(), badPath, root.c_str(), &detail, &error),
          ES_INVALID, ES_DIAG_API, "API_INVALID");
    check(es_load_json_bytes(runtime.get(), nullptr, 1, root.c_str(), &detail, &error),
          ES_INVALID, ES_DIAG_API, "API_INVALID");
    check(es_load_json_bytes(runtime.get(), nullptr, 0, root.c_str(), &detail, &error),
          ES_CONFIG, ES_DIAG_PARSE, "CFG_SYNTAX");
    require(detail.path[0] == 0 && detail.byte_offset == 0 && detail.line == 1 && detail.column == 1,
            "Byte-backed parse diagnostic has a wrong source or location");
    const auto load = [&](std::string_view bytes) {
        return es_load_json_bytes(runtime.get(), reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(),
                                  root.c_str(), &detail, &error);
    };
    const uint8_t one = '{';
    check(es_load_json_bytes(runtime.get(), &one, UINT64_MAX, root.c_str(), &detail, &error),
          ES_CAPACITY, ES_DIAG_API, "API_CAPACITY");
    check(es_load_json_bytes(runtime.get(), &one, (16u << 20) + 1, root.c_str(), &detail, &error),
          ES_CONFIG, ES_DIAG_PARSE, "CFG_LIMIT");
    check(load("{\"format_version\":3}"), ES_VERSION, ES_DIAG_SCHEMA, "CFG_VERSION");
    require(std::string_view(detail.pointer) == "/format_version", "Expected the rejected version pointer");
    const auto versionFailure = detail;
    es_diagnostic separate{};
    separate.size = sizeof(separate);
    separate.abi = ES_ABI_V2;
    const auto missing = (fixture.directory / "absent.json").u8string();
    require(es_load_json_file(runtime.get(), missing.c_str(), root.c_str(), &separate, &error) == ES_RESOURCE &&
            separate.stage == ES_DIAG_RESOURCE && separate.path == missing &&
            std::memcmp(&versionFailure, &detail, sizeof(detail)) == 0,
            "A second caller diagnostic clobbered the first");

    std::string key;
    for (int i = 0; i < 600; ++i) key += u8"é";
    auto withUnknown = fixture.document;
    withUnknown.insert(withUnknown.find('{') + 1, "\"" + key + "\":null,");
    check(load(withUnknown), ES_CONFIG, ES_DIAG_SCHEMA, "CFG_SHAPE");
    require(detail.truncated == (ES_DIAG_TRUNC_POINTER | ES_DIAG_TRUNC_FIELD) &&
            std::string_view(detail.pointer) == "/" + key.substr(0, 1022) &&
            std::string_view(detail.field) == key.substr(0, 126),
            "Truncated diagnostic must keep complete UTF-8 scalars");

    const auto differentSuffix = fixture.directory / "valid.data";
    Fixture::write(differentSuffix, fixture.document);
    const auto differentlyNamed = differentSuffix.u8string();
    check(es_load_json_file(runtime.get(), differentlyNamed.c_str(), root.c_str(), &detail, &error),
          ES_OK, ES_DIAG_NONE, "");
    require(detail.path[0] == 0 && detail.pointer[0] == 0 && detail.message[0] == 0 &&
            detail.byte_offset == ES_DIAG_NO_LOCATION && detail.line == ES_DIAG_NO_LOCATION &&
            detail.column == ES_DIAG_NO_LOCATION && error.message[0] == 0 && detail.truncated == 0,
            "Success did not clear the documented diagnostic fields");
}

void public_lifecycle(const Fixture &fixture) {
    const auto entry = fixture.entry.u8string();
    const auto root = fixture.directory.u8string();
    const auto impulse = fixture.directory / "impulse.wav";
    es_error error{};
    es_diagnostic detail{};
    detail.size = sizeof(detail);
    detail.abi = ES_ABI_V2;
    for (const auto mode : {ES_FREE, ES_DRIVEN}) {
        for (const auto rate : {44100u, 48000u}) {
            auto a = create(rate, mode), b = create(rate, mode);
            checked(es_load_json_file(a.get(), entry.c_str(), root.c_str(), &detail, &error), error);
            auto bytes = fixture.document;
            checked(es_load_json_bytes(b.get(), reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(),
                                       root.c_str(), &detail, &error), error);
            bytes.assign(bytes.size(), '!');
            advance(a.get(), b.get(), 0);

            controls(a.get(), mode == ES_DRIVEN, 1);
            controls(b.get(), mode == ES_DRIVEN, 1);
            const uint8_t malformed[] = {'{','\n','!'};
            require(es_load_json_bytes(a.get(), malformed, sizeof(malformed), root.c_str(), &detail, &error) == ES_CONFIG
                    && detail.stage == ES_DIAG_PARSE, "Malformed byte load did not report parse failure");
            same(a.get(), b.get());
            require(render(a.get(), 37) == render(b.get(), 37), "Failed load changed pending controls or frame phase");

            Fixture::write(fixture.entry, bytes);
            require(es_reset_detailed(a.get(), &detail, &error) == ES_RESOURCE && detail.stage == ES_DIAG_RESOURCE &&
                    std::string_view(detail.code) == "CFG_RESOURCE_BYTES", "Reset must reject changed file bytes before parsing");
            same(a.get(), b.get());
            Fixture::write(fixture.entry, fixture.document);
            engine_sim::RuntimeTestAccess::faultNextTick(*a);
            std::array<float,16> pcm;
            pcm.fill(42.0f);
            std::uint32_t written = 99;
            require(es_render(a.get(), pcm.size(), pcm.data(), pcm.size(), &written, &error) == ES_RENDER_FAULT &&
                    written < pcm.size() && std::all_of(pcm.begin() + written, pcm.end(), [](float value) { return value == 42.0f; }),
                    "Fault must publish only its completed prefix");
            const auto fault = telemetry(a.get());
            fs::remove(impulse);
            require(es_reset_detailed(a.get(), &detail, &error) == ES_RESOURCE && detail.path == impulse.u8string(),
                    "Reset must verify the original resource path while faulted");
            const auto after = telemetry(a.get());
            require(fault.state == ES_FAULTED && std::memcmp(&fault, &after, sizeof(fault)) == 0,
                    "Failed reset replaced its faulted snapshot");
            Fixture::write(impulse, fixture.impulse);
            checked(es_reset_detailed(a.get(), &detail, &error), error);
            fs::remove(fixture.entry);
            checked(es_reset(b.get(), &error), error);
            same(a.get(), b.get());
            require(telemetry(a.get()).frames == 0 && telemetry(a.get()).state == ES_READY,
                    "Successful reset must restart the session");
            advance(a.get(), b.get(), 2);
            checked(es_unload(a.get(), &error), error);
            checked(es_unload(a.get(), &error), error);
            require(es_reset(a.get(), &error) == ES_STATE, "Reset must reject EMPTY");
            Fixture::write(fixture.entry, fixture.document);
        }
    }
}
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    try {
        const Fixture fixture(fs::u8path(argv[2]), fs::u8path(argv[3]));
        const std::string_view mode = argv[1];
        if (mode == "transactional") transactional(fixture);
        else if (mode == "identity") identity(fixture);
        else if (mode == "v2-abi") public_abi(fixture);
        else if (mode == "v2-lifecycle") public_lifecycle(fixture);
        else return 2;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
