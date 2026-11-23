#include "capacity.hpp"
#include "json_session.hpp"
#include "runtime.hpp"

#ifdef ES_CAPACITY_MEASURE_ALLOCATIONS
#include "observe.hpp"
#endif

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>

namespace {
using Json = nlohmann::ordered_json;
namespace capacity = engine_sim::assembly;
namespace config = engine_sim::config_v1;
namespace runtime = engine_sim::json_runtime;
namespace fs = std::filesystem;
using Report = capacity::FleetCapacity;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string read(const fs::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    require(size > 0 && size < 16 * 1024 * 1024, "Missing or oversized capacity fixture: " + path.string());
    std::string result(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    require(static_cast<bool>(input.read(result.data(), static_cast<std::streamsize>(size))),
            "Incomplete capacity fixture");
    return result;
}

void write(const fs::path &path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(output.good(), "Could not write capacity fixture");
}

struct Sandbox {
    fs::path root = fs::temp_directory_path() / ("engine-sim-capacity-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Sandbox() { require(fs::create_directory(root), "Could not create capacity fixture directory"); }
    ~Sandbox() { std::error_code error; fs::remove_all(root, error); }
};

es_config profile(bool driven = false) {
    es_config value{};
    require(es_config_init(ES_ABI_V2, sizeof(value), &value, nullptr) == ES_OK, "Could not initialize raw profile");
    value.mode = driven ? ES_DRIVEN : ES_FREE;
    value.output_hz = driven ? 48000 : 44100;
    value.seed = 403;
    return value;
}

runtime::PreparedModelResult prepare(std::string_view bytes, const fs::path &root, Report &report,
                                     es_config options = profile(), const runtime::Checkpoint &checkpoint = {}) {
    return runtime::prepare_fleet_bytes(bytes.data(), bytes.size(), root, options, report, checkpoint);
}

runtime::PreparedModel accept(runtime::PreparedModelResult result) {
    if (const auto *issue = std::get_if<config::Diagnostic>(&result))
        throw std::runtime_error("Unexpected refusal " + issue->code + " " + issue->pointer + " " + issue->message);
    return std::get<runtime::PreparedModel>(std::move(result));
}

const config::Diagnostic &reject(const runtime::PreparedModelResult &result, std::string_view code,
                                 std::string_view field = {}) {
    const auto *issue = std::get_if<config::Diagnostic>(&result);
    require(issue && issue->code == code && (field.empty() || issue->field == std::string(field)),
        issue ? "Unexpected refusal " + issue->code + " (" + issue->field.value_or("") + "): " + issue->message
              : "Over-limit candidate was accepted");
    return *issue;
}

Json samples(std::size_t n) {
    Json result = Json::array();
    for (std::size_t i = 0; i < n; ++i) result.push_back({{"x", i}, {"y", 0}});
    return result;
}

Json curve(std::string id, std::size_t points) {
    return {{"id", std::move(id)}, {"kind", "samples"}, {"filter_radius", 1},
        {"input_scale", 1}, {"output_scale", 1}, {"samples", samples(points)}};
}

void append_little(std::string &bytes, std::uint32_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes.push_back(static_cast<char>((value >> (i * 8)) & 255));
}

std::string wave(std::size_t size, bool tail) {
    auto bytes = std::string("RIFF");
    append_little(bytes, static_cast<std::uint32_t>(size - 8), 4);
    bytes += "WAVEfmt ";
    append_little(bytes, 16, 4);
    append_little(bytes, 1, 2);
    append_little(bytes, 1, 2);
    append_little(bytes, 44100, 4);
    append_little(bytes, 88200, 4);
    append_little(bytes, 2, 2);
    append_little(bytes, 16, 2);
    bytes += "data";
    append_little(bytes, static_cast<std::uint32_t>(size - 44), 4);
    bytes.resize(size, '\0');
    bytes[44] = 100;
    if (tail) {
        bytes[size - 2] = static_cast<char>(155); // -101 in little-endian signed PCM.
        bytes[size - 1] = static_cast<char>(255);
    }
    return bytes;
}

void describe(std::string_view name, const Report &report, const engine_sim::JsonRecipe &recipe,
              std::uint64_t live, std::uint64_t peak) {
    Json inputs = Json::array();
    runtime::visit_file_inputs(recipe, [&](const fs::path &file, std::string_view bytes) {
        inputs.push_back({{"path", file.u8string()}, {"bytes", bytes.size()}});
    });
    const auto &shape = report.predicted;
    const auto &actual = report.assembled;
    std::cout << Json{{"model", name}, {"rate", report.output_hz}, {"simulation_hz", report.simulation_hz},
        {"seed", 403}, {"definitions", report.definitions}, {"curve_points", report.curve_points},
        {"cylinders", shape.cylinders}, {"cranks", shape.cranks}, {"banks", shape.banks},
        {"intakes", shape.intakes}, {"exhausts", shape.exhausts},
        {"predicted", {shape.bodies, shape.constraints, shape.rows, shape.generators}},
        {"observed", {actual.bodies, actual.constraints, actual.rows, actual.generators}},
        {"reserved_rows", shape.reserved_rows}, {"reserved_generators", shape.reserved_generators},
        {"fir", report.taps}, {"observed_fir", report.assembled_taps},
        {"delay", report.delay_lengths}, {"observed_delay", report.assembled_delay_lengths},
        {"delay_sum", report.delay_total}, {"tap_sum", report.tap_total},
        {"document_bytes", report.document_bytes}, {"pinned_wave_bytes", report.unique_wave_bytes},
        {"decoded_temporary_bytes", report.decoded_sample_bytes}, {"retained_bytes", report.retained_bytes},
        {"components_bytes", {{"recipe", report.recipe_bytes}, {"graph", report.graph_bytes},
            {"solver", report.solver_bytes}, {"delay", report.delay_bytes}, {"DSP", report.dsp_bytes},
            {"observed_DSP", report.assembled_dsp_bytes}, {"future_session", report.session_bytes},
            {"modeled_overhead", report.modeled_overhead}}},
        {"measured_cxx_live", live}, {"measured_cxx_peak", peak}, {"inputs", inputs}}.dump() << '\n';
}

Report observe(std::string_view name, const fs::path &model, const fs::path &root, const es_config &options) {
    Report report;
#ifdef ES_CAPACITY_MEASURE_ALLOCATIONS
    cost::reset_interval();
    const auto before = cost::sample().live_bytes;
    std::uint64_t live = 0, peak = 0;
#else
    const std::uint64_t live = 0, peak = 0;
#endif
    {
#ifdef ES_CAPACITY_MEASURE_ALLOCATIONS
        auto result = cost::observe(cost::Phase::preparation, [&] {
            return runtime::prepare_fleet_file(model, root, options, report);
        });
        live = cost::sample().live_bytes - before;
        peak = cost::sample().peak_live_bytes - before;
#else
        auto result = runtime::prepare_fleet_file(model, root, options, report);
#endif
        auto prepared = accept(std::move(result));
        describe(name, report, *prepared.recipe, live, peak);
#ifdef ES_CAPACITY_MEASURE_ALLOCATIONS
        require(live > 0 && peak >= live && report.retained_bytes >= live,
                "Retained model misses observed C++ allocations");
#endif
    }
#ifdef ES_CAPACITY_MEASURE_ALLOCATIONS
    require(cost::sample().live_bytes == before, "Capacity candidate retained private allocations after destruction");
#endif
    return report;
}

void initialize_shared(const fs::path &model, const fs::path &root, std::string_view format) {
    Report warm;
#ifdef ES_CAPACITY_MEASURE_ALLOCATIONS
    const auto previous = cost::sample().live_bytes;
    (void)accept(cost::observe(cost::Phase::preparation, [&] {
        return runtime::prepare_fleet_file(model, root, profile(), warm);
    }));
    const auto shared_bytes = cost::sample().live_bytes - previous;
    std::cout << Json{{"shared_schema_format", format}, {"incremental_shared_cxx_bytes", shared_bytes}}.dump() << '\n';
#else
    (void)format;
    (void)accept(runtime::prepare_fleet_file(model, root, profile(), warm));
#endif
}

void ledger(const fs::path &fixture) {
    initialize_shared(fixture, fixture.parent_path(), "v2 and Gaussian tables");
    for (const auto driven : {false, true}) {
        const auto single = observe("single", fixture, fixture.parent_path(), profile(driven));
        const auto &shape = single.predicted;
        require(shape.cylinders == 1 && shape.bodies == 4 && shape.constraints == 10 && shape.rows == 13 &&
            shape.exhausts == 1 && shape.generators == 1 + static_cast<std::size_t>(driven) &&
            single.taps[0] == 8 && single.delay_lengths[0] == 49 && single.dsp_bytes == single.assembled_dsp_bytes,
            "Single dimensions differ from independent capacity calculation");
    }
    const char *reference = std::getenv("ES_FLEET_REFERENCE");
    if (!reference) return; // Ordinary standalone CTest has only the redistributable fixture.
    const auto source = fs::path(reference) / "source";
    const auto kohler = source / "assets/engines/atg-video-1/02_kohler_ch750.json";
    const auto ls = source / "assets/engines/atg-video-2/07_gm_ls.json";
    initialize_shared(kohler, source, "v1");
    for (const auto driven : {false, true}) {
        const auto ch750 = observe("ch750", kohler, source, profile(driven));
        const auto gm = observe("gm-ls", ls, source, profile(driven));
        require(ch750.predicted.cylinders == 2 && ch750.predicted.exhausts == 1 &&
            gm.predicted.cylinders == 8 && gm.predicted.exhausts == 2, "Wrong reference topology");
        const auto ir = fs::file_size(source / "es/sound-library/smooth/smooth_39.wav");
        require(gm.unique_wave_bytes == ir && gm.tap_total == 2 * gm.taps[0] && gm.taps[0] == gm.taps[1],
                "Shared IR was not deduplicated by source and charged per channel");
    }
    Sandbox scratch;
    auto aliased = Json::parse(read(ls));
    const auto &original_impulse = aliased["components"]["impulses"][0];
    auto second = original_impulse;
    const auto source_ir = source / original_impulse.at("path").get<std::string>();
    const auto bytes = read(source_ir);
    aliased["components"]["impulses"][0]["path"] = "impulse.wav";
    second["id"] = "right-ir";
    second["path"] = "impulse.wav";
    write(scratch.root / "impulse.wav", bytes);
    std::error_code symlink_error;
    fs::create_symlink("impulse.wav", scratch.root / "alias.wav", symlink_error);
    if (!symlink_error) second["path"] = "alias.wav";
    aliased["components"]["impulses"].push_back(second);
    aliased["components"]["exhausts"][1]["impulse_response"] = "right-ir";
    Report report;
    auto model = accept(prepare(aliased.dump(), scratch.root, report));
    require(report.unique_wave_bytes == bytes.size() && report.tap_total == 2 * report.taps[0],
            "Two routed locators bound to one canonical WAVE were charged incorrectly");
    std::size_t count = 0;
    runtime::visit_file_inputs(*model.recipe, [&](const fs::path &file, std::string_view actual) {
        require(file == fs::canonical(scratch.root / "impulse.wav") && actual == bytes, "Pinned WAVE identity changed");
        ++count;
    });
    require(count == 1, "An alias added an extra resource input");
}

void limits(const Json &base, const Sandbox &scratch) {
    Report report;
    const auto path = scratch.root / "input.json";
    auto padded = base.dump();
    padded.resize(capacity::fleet_document_limit, ' ');
    write(path, padded);
    (void)accept(runtime::prepare_fleet_file(path, scratch.root, profile(), report));
    require(report.document_bytes == capacity::fleet_document_limit, "File accepted at wrong byte extent");
    (void)accept(prepare(padded, scratch.root, report));
    padded += ' ';
    write(path, padded);
    reject(runtime::prepare_fleet_file(path, scratch.root, profile(), report), "CFG_LIMIT", "document bytes");
    reject(prepare(padded, scratch.root, report), "CFG_LIMIT", "document bytes");
    reject(runtime::prepare_fleet_bytes("", std::numeric_limits<std::uint64_t>::max(), scratch.root, profile(), report),
           "CFG_LIMIT", "document bytes");
    reject(runtime::prepare_fleet_bytes(nullptr, 1, scratch.root, profile(), report), "API_INVALID");
    reject(runtime::prepare_fleet_file("relative.json", scratch.root, profile(), report), "API_INVALID");

    auto extended = base;
    auto &curves = extended["components"]["functions"];
    const auto starting_definitions = report.definitions + base.at("port_flow_tables").size();
    for (std::size_t i = starting_definitions; i < capacity::fleet_definition_limit; ++i)
        curves.push_back(curve("unused-" + std::to_string(i), 1));
    // The fleet count admits exactly 256 declarations. The inherited graph
    // rule still rejects unused definitions after running that preflight.
    reject(prepare(extended.dump(), scratch.root, report), "CFG_REF");
    curves.push_back(curve("one-too-many", 1));
    reject(prepare(extended.dump(), scratch.root, report), "CFG_LIMIT", "definitions");

    auto with_samples = base;
    auto &more = with_samples["components"]["functions"];
    (void)accept(prepare(base.dump(), scratch.root, report));
    const auto other_points = report.curve_points - 20; // 3 port, 5 cam, 2 timing, 10 fuel.
    for (unsigned i = 0; i < 4; ++i)
        more[i] = curve(more[i].at("id").get<std::string>(), capacity::fleet_curve_limit - (i == 3 ? other_points : 0));
    (void)accept(prepare(with_samples.dump(), scratch.root, report));
    require(report.curve_points == capacity::fleet_point_limit, "Wrong total point accounting");
    more.push_back(curve("one-point-too-many", 1));
    reject(prepare(with_samples.dump(), scratch.root, report), "CFG_LIMIT", "samples");
    more = base["components"]["functions"];
    const auto cam_id = more[1].at("id").get<std::string>();
    more[1] = curve(cam_id, capacity::fleet_curve_limit + 1);
    reject(prepare(with_samples.dump(), scratch.root, report), "CFG_LIMIT", "samples");
    more[1] = {{"id", cam_id}, {"kind", "harmonic_cam_lobe"},
        {"duration_at_50_thou_deg", 234}, {"max_lift_thou", 551}, {"steps", 513}};
    reject(prepare(with_samples.dump(), scratch.root, report), "CFG_LIMIT", "steps");

    auto raw = profile();
    es_runtime *handle = nullptr;
    require(es_create(&raw, &handle, nullptr) == ES_OK, "Raw runtime did not accept its public profile");
    std::unique_ptr<es_runtime, decltype(&es_destroy)> owner(handle, es_destroy);
    require(runtime::load_bytes(handle, padded.data(), padded.size(), scratch.root) == ES_OK,
        "Raw v2 accidentally inherited the fleet document limit");
    const auto over_curve = with_samples.dump();
    require(runtime::load_bytes(handle, over_curve.data(), over_curve.size(), scratch.root) == ES_OK,
        "Raw v2 accidentally inherited the fleet expansion limit");

    auto invalid = profile();
    invalid.mode = 0xffffffffu;
    reject(prepare(base.dump(), scratch.root, report, invalid), "API_INVALID", "mode");
    invalid = profile();
    invalid.output_hz = 96000;
    reject(prepare(base.dump(), scratch.root, report, invalid), "CFG_LIMIT", "output_hz");
    for (std::uint32_t hz : {4000u, 96000u}) {
        auto rate = profile();
        rate.simulation_hz = hz;
        (void)accept(prepare(base.dump(), scratch.root, report, rate));
        require(report.simulation_hz == hz, "Simulation rate override was lost");
    }
    invalid = profile();
    invalid.simulation_hz = 96001;
    reject(prepare(base.dump(), scratch.root, report, invalid), "CFG_INITIALIZE");
}

void arithmetic() {
    capacity::FleetShape exact{12, 2, 9, 4, 2};
    require(!capacity::check_fleet_shape(exact, true) && exact.bodies == 27 && exact.constraints == 46 &&
            exact.rows == 72 && exact.generators == 13, "Exact shape boundary mismatch");
    for (const auto member : {&capacity::FleetShape::cylinders, &capacity::FleetShape::cranks,
            &capacity::FleetShape::banks, &capacity::FleetShape::intakes, &capacity::FleetShape::exhausts}) {
        auto wrong = exact;
        ++(wrong.*member);
        require(capacity::check_fleet_shape(wrong, true).has_value(), "Shape ceiling accepted one above");
        wrong.*member = 0;
        require(capacity::check_fleet_shape(wrong, true).has_value(), "Zero shape accepted");
    }
    require(capacity::fleet_cells(1) == 4 && capacity::fleet_cells(8) == 11 && !capacity::fleet_cells(9) &&
        !capacity::fleet_cells(0) && !capacity::fleet_cells(std::numeric_limits<std::size_t>::max()), "Roster boundary mismatch");
    std::size_t bytes = 0;
    require(capacity::charge_capacity(bytes, 16, 1024 * 1024, capacity::fleet_cell_limit) && bytes == capacity::fleet_cell_limit &&
        !capacity::charge_capacity(bytes, 1, 1, capacity::fleet_cell_limit) && bytes == capacity::fleet_cell_limit &&
        !capacity::charge_capacity(bytes, std::numeric_limits<std::size_t>::max(), 2), "Cell byte arithmetic wrapped");

    Report delay;
    const auto length = [](std::size_t samples) { return (samples - 32) * (343.0 / 10000.0); };
    for (std::size_t i = 0; i < 7; ++i)
        require(!capacity::assign_delay(delay, i, length(1024), 0, "bank"), "Exact per-cylinder delay rejected");
    require(!capacity::assign_delay(delay, 7, length(992), 0, "bank"), "Exact delay contribution rejected");
    require(capacity::assign_delay(delay, 8, length(33), 0, "bank").has_value(), "8193 delay slots accepted");
    require(!capacity::assign_delay(delay, 8, .001, 0, "bank") && delay.delay_total == 8192,
            "8192 delay slots rejected");
    require(capacity::assign_delay(delay, 9, length(1025), 0, "bank")->field == "delay ring samples",
            "1025 per-cylinder delay accepted");
    for (const auto unusable : {-1.0, 1.0e50, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        require(capacity::assign_delay(delay, 9, unusable, 0, "bank").has_value(), "Invalid delay converted to a signed integer");
    Report filters;
    require(!capacity::assign_taps(filters, 0, 10000, "left") && !capacity::assign_taps(filters, 1, 10000, "right") &&
        filters.tap_total == 20000 && capacity::assign_taps(filters, 1, 10001, "right").has_value(),
        "FIR sum/individual boundaries mismatch");
}

void transactional(const Json &base, const Sandbox &scratch) {
    const auto fixture = base.dump();
    auto options = profile();
    es_runtime *left = nullptr, *right = nullptr;
    require(es_create(&options, &left, nullptr) == ES_OK, "Could not create active runtime");
    std::unique_ptr<es_runtime, decltype(&es_destroy)> active(left, es_destroy);
    require(es_create(&options, &right, nullptr) == ES_OK, "Could not create untouched twin");
    std::unique_ptr<es_runtime, decltype(&es_destroy)> twin(right, es_destroy);
    for (auto *voice : {active.get(), twin.get()}) {
        require(runtime::load_bytes(voice, fixture.data(), fixture.size(), scratch.root) == ES_OK, "Failed to start raw voice");
        es_controls controls{sizeof(es_controls), ES_ABI_V2, 1, 1, .25, .5, 0};
        require(es_set_controls(voice, &controls, nullptr) == ES_OK, "Failed to set active voice controls");
    }
    const auto same = [&] {
        std::array<float, 2048> a{}, b{};
        std::uint32_t written = 0;
        require(es_render(active.get(), a.size(), a.data(), a.size(), &written, nullptr) == ES_OK && written == a.size(), "Active voice failed");
        require(es_render(twin.get(), b.size(), b.data(), b.size(), &written, nullptr) == ES_OK && written == b.size(), "Twin failed");
        require(std::memcmp(a.data(), b.data(), sizeof(a)) == 0, "Preparation changed PCM of an active voice");
        es_telemetry active_state{};
        active_state.size = sizeof(es_telemetry);
        active_state.abi = ES_ABI_V2;
        auto twin_state = active_state;
        require(es_get_telemetry(active.get(), &active_state, nullptr) == ES_OK &&
            es_get_telemetry(twin.get(), &twin_state, nullptr) == ES_OK &&
            std::memcmp(&active_state, &twin_state, sizeof(active_state)) == 0,
            "Preparation changed counters or requested state of an active voice");
    };
    same();
    Report report;
    const auto ir = scratch.root / "impulse.wav";
    const auto original = read(ir);
    auto malformed = base;
    malformed["components"]["banks"][0]["cylinders"][0]["ignition_wire"] = "unresolved-wire";
    reject(prepare(malformed.dump(), scratch.root, report), "CFG_REF");
    malformed = base;
    malformed["components"]["banks"][0]["cylinders"][0]["primary_length"] = 1000;
    reject(prepare(malformed.dump(), scratch.root, report), "CFG_LIMIT", "delay ring samples");
    malformed = base;
    malformed["components"]["rods"][0]["length"] = .0001;
    reject(prepare(malformed.dump(), scratch.root, report), "CFG_INITIALIZE");
    same();

    const auto full_wave = wave(capacity::fleet_wave_limit, true);
    write(ir, full_wave);
    (void)accept(prepare(base.dump(), scratch.root, report));
    require(report.unique_wave_bytes == capacity::fleet_wave_limit && report.taps[0] == 10000,
            "Exact pinned WAVE budget or effective FIR length mismatch");
    write(ir, full_wave + 'x');
    reject(prepare(base.dump(), scratch.root, report), "CFG_LIMIT", "path");
    write(ir, wave(62, false));
    (void)accept(prepare(base.dump(), scratch.root, report));
    require(report.taps[0] == 0, "Exact threshold should not contribute wet FIR samples");
    write(ir, original);
    struct Canceled {};
    for (auto phase : {runtime::Phase::document, runtime::Phase::normalized, runtime::Phase::prepared}) {
        bool stopped = false;
        try {
            (void)prepare(fixture, scratch.root, report, options, [&](runtime::Phase checkpoint) {
                if (checkpoint == phase) throw Canceled{};
            });
        } catch (const Canceled &) { stopped = true; }
        require(stopped, "Cancellation checkpoint was not visited");
        same();
    }
    require(runtime::reset(active.get()) == ES_OK && runtime::reset(twin.get()) == ES_OK,
        "Byte and canonical resource identity were not retained by the active voice");
    same();
    std::cout << "Exact and one-above checks passed for document, definitions, points, geometry, delay, WAVE, FIR, "
                 "shape, roster and cell byte arithmetic. Active and twin PCM/telemetry match after every failure phase.\n";
}
}

int main(int argc, char **argv) try {
    require(argc == 3, "Expected suite and fixture path");
    const auto fixture = fs::absolute(argv[2]);
    if (std::string_view(argv[1]) == "ledger") {
        ledger(fixture);
    } else {
        Sandbox scratch;
        write(scratch.root / "impulse.wav", read(fixture.parent_path() / "impulse.wav"));
        const auto base = Json::parse(read(fixture));
        arithmetic();
        limits(base, scratch);
        transactional(base, scratch);
    }
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
