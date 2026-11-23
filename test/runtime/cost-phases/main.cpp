#include "observe.hpp"
#include <engine_sim/runtime.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif
#ifdef __linux__
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <locale>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using cost::Phase;
using Handle = std::unique_ptr<es_runtime, decltype(&es_destroy)>;
constexpr std::size_t observation_frames = 8192;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void checked(es_status status, const es_error &error) {
    require(status == ES_OK, error.message);
}

const cost::Counts &phase(const cost::Sample &sample, Phase phase) {
    return sample.phases[static_cast<std::size_t>(phase)];
}

std::pair<std::int64_t, std::uint64_t> rss() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    require(GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)) != 0, "Cannot read working set");
    return {static_cast<std::int64_t>(memory.WorkingSetSize), memory.PeakWorkingSetSize};
#else
    rusage data{};
    require(getrusage(RUSAGE_SELF, &data) == 0, "Cannot read peak RSS");
#ifdef __linux__
    std::int64_t address_pages, resident_pages;
    std::ifstream statm("/proc/self/statm");
    const auto size = sysconf(_SC_PAGESIZE);
    require(size > 0 && bool(statm >> address_pages >> resident_pages), "Cannot read resident pages");
    return {resident_pages * size, static_cast<std::uint64_t>(data.ru_maxrss) * 1024};
#elif defined(__APPLE__)
    return {-1, static_cast<std::uint64_t>(data.ru_maxrss)};
#else
#error Specify memory accounting units for this platform.
#endif
#endif
}

void write(std::ostream &file, std::string_view operation) {
    const auto &sample = cost::sample();
    const auto resident = rss();
    for (std::size_t index = 0; index < sample.phases.size(); ++index) {
        const auto &c = sample.phases[index];
        file << operation << ',' << cost::labels[index] << ',' << c.calls << ',' << c.ns << ','
             << c.allocations << ',' << c.allocated_bytes << ',' << c.live_objects << ',' << c.live_bytes << ','
             << c.locks << ',' << sample.live_objects << ',' << sample.live_bytes << ',' << sample.peak_live_bytes
             << ',' << sample.swap_live_bytes << ',' << sample.swaps << ',' << resident.first << ',' << resident.second << '\n';
    }
    file.flush();
    require(bool(file), "Cannot write preparation accounting");
}

Handle create(unsigned rate, bool driven, unsigned seed) {
    es_config config{};
    es_error error{};
    checked(es_config_init(ES_ABI_V2, sizeof(config), &config, &error), error);
    config.output_hz = rate;
    config.mode = driven ? ES_DRIVEN : ES_FREE;
    config.seed = seed;
    es_runtime *runtime = nullptr;
    const auto status = cost::observe(Phase::owner, [&] { return es_create(&config, &runtime, &error); });
    checked(status, error);
    return Handle(runtime, es_destroy);
}

struct Input {
    const std::string &entry, &root;
    bool from_bytes;
    std::vector<std::uint8_t> bytes;

    void load(es_runtime *runtime) const {
        es_error error{};
        const auto status = cost::observe(Phase::preparation, [&] {
            if (from_bytes) {
                return es_load_json_bytes(runtime, bytes.data(), bytes.size(), root.c_str(), nullptr, &error);
            }
            return es_load_json_file(runtime, entry.c_str(), root.c_str(), nullptr, &error);
        });
        checked(status, error);
    }
};

void reset(es_runtime *runtime) {
    es_error error{};
    const auto status = cost::observe(Phase::preparation, [&] { return es_reset(runtime, &error); });
    checked(status, error);
}

es_telemetry telemetry(es_runtime *runtime) {
    es_telemetry value{};
    value.size = sizeof(value);
    value.abi = ES_ABI_V2;
    es_error error{};
    checked(es_get_telemetry(runtime, &value, &error), error);
    return value;
}

void unload(es_runtime *runtime) {
    es_error error{};
    checked(es_unload(runtime, &error), error);
    require(telemetry(runtime).state == ES_EMPTY, "Unload left a ready session");
}

template<std::size_t Size>
std::array<float, Size> render(es_runtime *runtime, bool driven, bool steady = false) {
    std::array<float, Size> result;
    es_error error{};
    const auto before = telemetry(runtime);
    const std::uint64_t spin_start = before.output_hz / 10;
    for (std::uint32_t offset = 0; offset < Size;) {
        const auto cursor = before.frames + offset;
        const bool waiting = driven && cursor < spin_start;
        const auto limit = static_cast<std::uint32_t>(Size) - offset;
        const auto count = waiting ? std::min(limit, static_cast<std::uint32_t>(spin_start - cursor)) : limit;
        const es_controls controls{sizeof(controls), ES_ABI_V2, driven ? 0U : 1U, driven ? 0U : 1U,
                                   driven ? .10 : .30, 0, driven && !waiting ? 600.0 : 0.0};
        checked(es_set_controls(runtime, &controls, &error), error);
        std::uint32_t written = 0;
        const auto pull = [&] { return es_render(runtime, count, result.data() + offset, count, &written, &error); };
        checked(steady ? cost::observe(Phase::render, pull) : pull(), error);
        require(written == count, "Incomplete PCM");
        offset += written;
    }
    const auto after = telemetry(runtime);
    require(after.frames == before.frames + Size, "Incomplete PCM clock");
    require(after.ticks == after.frames * after.simulation_hz / after.output_hz &&
            after.native_frames == (after.frames * 44100 + after.output_hz - 1) / after.output_hz,
            "Preparation affected the runtime clock");
    require(std::all_of(result.begin(), result.end(), [](float sample) { return std::isfinite(sample); }),
            "Nonfinite PCM");
    return result;
}

void check_ready(bool from_bytes) {
    const auto &s = cost::sample();
    const auto doc = from_bytes ? Phase::document_copy : Phase::document_read;
    for (auto stage : {Phase::preparation, doc, Phase::tokenize_parse, Phase::schema,
                       Phase::document_projection, Phase::normalization, Phase::semantic_graph,
                       Phase::assembly, Phase::definition, Phase::resource_io, Phase::resource_decode,
                       Phase::geometry_solver, Phase::dsp, Phase::convolution, Phase::activation}) {
        const auto &observed = phase(s, stage);
        require(observed.calls && observed.ns, "Missing preparation phase measurements");
    }
    for (auto temporary : {Phase::tokenize_parse, Phase::schema, Phase::document_projection,
                            Phase::normalization, Phase::semantic_graph, Phase::resource_decode}) {
        require(!phase(s, temporary).live_objects, "Transient preparation storage survived activation");
    }
    for (auto retained : {doc, Phase::resource_io, Phase::definition, Phase::geometry_solver,
                           Phase::dsp, Phase::convolution, Phase::activation}) {
        require(phase(s, retained).live_bytes > 0, "Missing retained preparation allocation category");
    }
    require(s.swaps == 1 && s.peak_live_bytes >= s.swap_live_bytes && s.swap_live_bytes >= s.live_bytes,
            "Missing pre-swap high-water sample");
}

int run(const std::vector<std::string> &args) {
    require(args.size() == 8, "Expected entry, resource root, output CSV, file|bytes, rate, free|driven, cycle count");
    require(args[4] == "file" || args[4] == "bytes", "Expected file or bytes input");
    require(args[5] == "44100" || args[5] == "48000", "Expected a supported output rate");
    require(args[6] == "free" || args[6] == "driven", "Expected free or driven mode");
    require(args[7] == "10" || args[7] == "100", "Expected 10 smoke cycles or 100 study cycles");
    const bool from_bytes = args[4] == "bytes";
    const bool driven = args[6] == "driven";
    const unsigned rate = static_cast<unsigned>(std::stoul(args[5]));
    const unsigned cycles = static_cast<unsigned>(std::stoul(args[7]));
    Input input{args[1], args[2], from_bytes, {}};
    if (from_bytes) {
        std::ifstream file(std::filesystem::u8path(input.entry), std::ios::binary);
        require(bool(file), "Cannot open byte input");
        input.bytes.assign(std::istreambuf_iterator<char>(file), {});
        require(!input.bytes.empty(), "Empty byte input");
    }
    const auto output = std::filesystem::u8path(args[3]);
    require(!std::filesystem::exists(output), "Measurement output already exists");
    std::ofstream log(output);
    log.imbue(std::locale::classic());
    log << "operation,phase,calls,wall_ns,allocations,allocated_bytes,live_objects,live_bytes,locks,"
           "total_live_objects,total_live_bytes,peak_live_bytes,swap_live_bytes,swaps,rss_bytes,peak_rss_bytes\n";
    auto a = create(rate, driven, 403);
    const auto empty = cost::sample().live_bytes;
    write(log, "empty");

    cost::reset_interval();
    input.load(a.get());
    write(log, "cold");
    check_ready(from_bytes);
    const auto ready = cost::sample().live_bytes;
    const auto schema = phase(cost::sample(), Phase::shared_schema);
    const auto filter = phase(cost::sample(), Phase::shared_filter);
    require(schema.calls == 1 && schema.live_bytes > 0 && filter.calls == 1 && filter.live_bytes > 0,
            "Missing immutable shared tables");
    const auto shared_bytes = schema.live_bytes + filter.live_bytes;
    const auto shared_objects = schema.live_objects + filter.live_objects;
    const auto audio = render<observation_frames>(a.get(), driven);
    require(std::any_of(audio.begin(), audio.end(), [](float sample) { return sample != 0; }), "Silent preparation PCM");

    cost::reset_interval();
    (void)render<observation_frames>(a.get(), driven, true);
    write(log, "steady");
    const auto steady = phase(cost::sample(), Phase::render);
    require(steady.calls == 1 && steady.allocations == 0 && steady.locks == 0,
            "Steady runtime made allocations or took a wrapped lock");

    cost::reset_interval();
    input.load(a.get());
    write(log, "warm_replacement");
    check_ready(from_bytes);
    require(cost::sample().swap_live_bytes > ready && cost::sample().live_bytes == ready,
            "Replacement failed to account for both sessions or retained additional objects");
    require(render<observation_frames>(a.get(), driven) == audio, "Replacement changed PCM");

    cost::reset_interval();
    reset(a.get());
    write(log, "reset");
    check_ready(from_bytes);
    require(cost::sample().swap_live_bytes > ready && cost::sample().live_bytes == ready,
            "Reset failed to account for both sessions or retained additional objects");
    require(render<observation_frames>(a.get(), driven) == audio, "Reset changed PCM");

    cost::reset_interval();
    const auto warmed = cost::sample().live_bytes;
    auto b = create(rate, driven, 404);
    input.load(b.get());
    write(log, "two_owners");
    check_ready(from_bytes);
    require(cost::sample().live_bytes == warmed + ready - shared_bytes,
            "Two independent owners failed live accounting");
    (void)render<257>(b.get(), driven);
    b.reset();
    require(cost::sample().live_bytes == warmed, "Second owner retained storage after destroy");
    unload(a.get());
    write(log, "unloaded");
    require(cost::sample().live_bytes == empty + shared_bytes, "Unload retained session allocations");
    a.reset();

    for (unsigned cycle = 1; cycle <= cycles; ++cycle) {
        cost::reset_interval();
        auto runtime = create(rate, driven, 403);
        input.load(runtime.get());
        const auto before_reset = render<257>(runtime.get(), driven);
        reset(runtime.get());
        require(render<257>(runtime.get(), driven) == before_reset, "Lifecycle reset changed PCM");
        unload(runtime.get());
        require(cost::sample().live_bytes == empty + shared_bytes, "Cycle unload retained session storage");
        runtime.reset();
        require(cost::sample().live_objects == shared_objects &&
                cost::sample().live_bytes == shared_bytes && !phase(cost::sample(), Phase::shared_schema).calls &&
                !phase(cost::sample(), Phase::shared_filter).calls,
                "Cycle retained owner or validator storage");
        write(log, "cycle_" + std::to_string(cycle));
    }
    auto audio_path = output;
    audio_path += ".f32le";
    std::ofstream pcm(audio_path, std::ios::binary);
    pcm.write(reinterpret_cast<const char *>(audio.data()), sizeof(audio));
    pcm.close();
    require(bool(pcm), "Cannot write preparation PCM");
    return 0;
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    try {
        std::vector<std::string> args;
        for (int i = 0; i < argc; ++i) args.push_back(std::filesystem::path(argv[i]).u8string());
        return run(args);
#else
int main(int argc, char **argv) {
    try {
        return run({argv, argv + argc});
#endif
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
