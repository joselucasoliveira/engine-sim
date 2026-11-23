#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif
#include "runtime.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <mutex>
#include <new>
#include <sstream>
#include <thread>
#include <vector>

#ifndef ES_BENCH_PROBE
#define ES_BENCH_PROBE 0
#endif
#ifndef ES_BENCH_MUTEX_WRAP
#define ES_BENCH_MUTEX_WRAP 0
#endif
namespace bench {
using Clock = std::chrono::steady_clock;
struct Counts { std::uint64_t allocations = 0, bytes = 0, locks = 0; };
#if ES_BENCH_PROBE
thread_local Counts *activeCounts = nullptr;
struct CountScope {
    Counts *previous;
    explicit CountScope(Counts &value) : previous(activeCounts) { activeCounts = &value; }
    ~CountScope() { activeCounts = previous; }
};
#else
struct CountScope { explicit CountScope(Counts &) {} };
#endif
double elapsed(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }
struct Usage { double cpu = 0; std::uint64_t current = 0, peak = 0; };
Usage usage() {
    Usage value;
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) throw std::runtime_error("Cannot read process CPU time");
    const auto seconds = [](FILETIME time) { ULARGE_INTEGER bits; bits.LowPart = time.dwLowDateTime; bits.HighPart = time.dwHighDateTime; return bits.QuadPart * 1e-7; };
    value.cpu = seconds(kernel) + seconds(user);
    PROCESS_MEMORY_COUNTERS memory{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) throw std::runtime_error("Cannot read process memory");
    value.current = memory.WorkingSetSize; value.peak = memory.PeakWorkingSetSize;
#else
    rusage data{};
    if (getrusage(RUSAGE_SELF, &data) != 0) throw std::runtime_error("Cannot read process CPU/memory");
    value.cpu = data.ru_utime.tv_sec + data.ru_utime.tv_usec * 1e-6 + data.ru_stime.tv_sec + data.ru_stime.tv_usec * 1e-6;
#ifdef __APPLE__
    value.peak = data.ru_maxrss;
#else
    value.peak = static_cast<std::uint64_t>(data.ru_maxrss) * 1024;
    std::ifstream statm("/proc/self/statm"); std::uint64_t virtualPages, residentPages;
    const auto page = sysconf(_SC_PAGESIZE);
    if (!(statm >> virtualPages >> residentPages) || page <= 0) throw std::runtime_error("Cannot read current process RSS");
    value.current = residentPages * static_cast<std::uint64_t>(page);
#endif
#endif
    return value;
}
}
#if ES_BENCH_PROBE
#if defined(_MSC_VER)
#define ES_BENCH_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define ES_BENCH_NOINLINE __attribute__((noinline))
#else
#define ES_BENCH_NOINLINE
#endif
ES_BENCH_NOINLINE void *operator new(std::size_t bytes) {
    if (bench::activeCounts) { ++bench::activeCounts->allocations; bench::activeCounts->bytes += bytes; }
    if (auto *pointer = std::malloc(bytes ? bytes : 1)) return pointer;
    throw std::bad_alloc();
}
ES_BENCH_NOINLINE void operator delete(void *pointer) noexcept { std::free(pointer); }
ES_BENCH_NOINLINE void operator delete(void *pointer, std::size_t) noexcept { std::free(pointer); }
void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void *pointer) noexcept { ::operator delete(pointer); }
void operator delete[](void *pointer, std::size_t) noexcept { ::operator delete(pointer); }
#if ES_BENCH_MUTEX_WRAP
#include <pthread.h>
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t *);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (bench::activeCounts) ++bench::activeCounts->locks;
    return __real_pthread_mutex_lock(mutex);
}
#endif
#endif

namespace engine_sim {
struct RuntimeTestAccess {
    static PullEngine &pull(es_runtime &handle) { return *handle.implementation.m_session->pull; }
    static const Simulator &simulator(const es_runtime &handle) { return *handle.implementation.m_session->prepared.session->simulator; }
    static const Synthesizer &synthesis(const es_runtime &handle) { return handle.implementation.m_session->prepared.session->synthesis; }
    static atg_scs::RigidBodySystem &system(es_runtime &handle) { return *handle.implementation.m_session->prepared.session->simulator->getSystem(); }
};
struct ClockTestAccess {
    static ClockSource &source(PullEngine &pull) { return static_cast<ClockSource &>(pull); }
};
}
namespace bench {
namespace fs = std::filesystem;
using Handle = std::unique_ptr<es_runtime, decltype(&es_destroy)>;
void checked(es_status status, const es_error &error) { if (status != ES_OK) throw std::runtime_error(error.message); }
struct Options {
    fs::path entry, assets, output;
    unsigned rate, block, instances, warmBlocks, measureBlocks;
    std::string pipeline, schedule;
    std::uint64_t seed;
};
std::uint64_t integer(const std::string &value) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) throw std::invalid_argument("Expected an unsigned decimal argument");
    std::size_t used = 0; const auto number = std::stoull(value, &used);
    if (used != value.size()) throw std::invalid_argument("Incomplete integer argument");
    return number;
}
unsigned integer32(const std::string &value) {
    const auto number = integer(value);
    if (number > UINT32_MAX) throw std::invalid_argument("Benchmark argument exceeds uint32");
    return static_cast<unsigned>(number);
}
Options options(const std::vector<std::string> &args) {
    if (args.size() != 12) throw std::invalid_argument("Expected model asset-root output rate block instances pipeline warm-blocks measure-blocks seed schedule");
    Options value{fs::u8path(args[1]), fs::u8path(args[2]), fs::u8path(args[3]),
        integer32(args[4]), integer32(args[5]), integer32(args[6]),
        integer32(args[8]), integer32(args[9]), args[7], args[11], integer(args[10])};
    if ((value.rate != 44100 && value.rate != 48000) || value.block == 0 || value.block > ES_MAX_FRAMES ||
        value.instances == 0 || value.instances > 2 || value.warmBlocks < 150 || value.warmBlocks > 500 ||
        value.measureBlocks == 0 || value.measureBlocks > 100 ||
        (value.pipeline != "public" && value.pipeline != "core" && value.pipeline != "profile" && value.pipeline != "convert" && value.pipeline != "cycles") ||
        (value.schedule != "legacy" && value.schedule != "fixture")) throw std::invalid_argument("Unsupported benchmark workload");
    if (value.schedule == "legacy" && value.warmBlocks + value.measureBlocks > 300)
        throw std::invalid_argument("Legacy benchmark interval must precede the 3-second demand ramp");
#if !ES_BENCH_PROBE
    if (value.pipeline == "profile") throw std::invalid_argument("Stage clocks require the separate diagnostic probe");
#endif
    for (const auto &path : {value.entry, value.assets, value.output}) if (!path.is_absolute()) throw std::invalid_argument("Benchmark paths must be absolute");
    if (fs::exists(value.output)) throw std::invalid_argument("Benchmark output already exists");
    return value;
}
struct Stages {
    double physics = 0, synthesis = 0, latch = 0;
    std::uint64_t ticks = 0, native = 0;
    bool enabled = false;
};
class SourceProbe final : public engine_sim::ClockSource {
public:
    SourceProbe(engine_sim::ClockSource &source, Stages &stages) : m_source(source), m_stages(stages) {}
    void latch() override {
        if (!m_stages.enabled) { m_source.latch(); return; }
        const auto start = Clock::now(); m_source.latch(); m_stages.latch += elapsed(start);
    }
    bool tick(double *endpoint, std::size_t capacity) override {
        if (!m_stages.enabled) return m_source.tick(endpoint, capacity);
        const auto start = Clock::now();
        const bool result = m_source.tick(endpoint, capacity);
        m_stages.physics += elapsed(start);
        ++m_stages.ticks;
        return result;
    }
    bool native(engine_sim::AcousticExcitation input, std::int16_t &sample) override {
        if (!m_stages.enabled) return m_source.native(input, sample);
        const auto start = Clock::now(); const bool result = m_source.native(input, sample); m_stages.synthesis += elapsed(start); ++m_stages.native; return result;
    }
private:
    engine_sim::ClockSource &m_source;
    Stages &m_stages;
};
struct Stats {
    double create = 0, load = 0, reset = 0, reload = 0, render = 0;
    Counts createCounts, loadCounts, resetCounts, reloadCounts, renderCounts;
    engine_sim::ClockCounts before, after;
    double speed = 0;
    std::array<double, 4> solverAverages{};
    std::uint64_t nonzero = 0;
};
class Instance {
public:
    explicit Instance(const Options &options) : m_options(options), m_handle(nullptr, es_destroy),
        m_output(options.rate / 100 * options.measureBlocks) {
        es_config config{}; es_error error{}; checked(es_config_init(ES_ABI_V2, sizeof(config), &config, &error), error);
        config.output_hz = options.rate; config.seed = options.seed;
        es_runtime *raw = nullptr; auto start = Clock::now(); es_status status;
        { CountScope count(stats.createCounts); status = es_create(&config, &raw, &error); }
        stats.create = elapsed(start); checked(status, error); m_handle.reset(raw);
        const auto entry = options.entry.u8string(), assets = options.assets.u8string();
        start = Clock::now();
        { CountScope count(stats.loadCounts); status = es_load_json_file(raw, entry.c_str(), assets.c_str(), nullptr, &error); }
        stats.load = elapsed(start); checked(status, error);
        start = Clock::now();
        { CountScope count(stats.resetCounts); status = es_reset(raw, &error); }
        stats.reset = elapsed(start); checked(status, error);
        checked(es_unload(raw, &error), error); start = Clock::now();
        { CountScope count(stats.reloadCounts); status = es_load_json_file(raw, entry.c_str(), assets.c_str(), nullptr, &error); }
        stats.reload = elapsed(start); checked(status, error);
        es_telemetry metadata{}; metadata.size = sizeof(metadata); metadata.abi = ES_ABI_V2;
        checked(es_get_telemetry(raw, &metadata, &error), error); m_frequency = metadata.simulation_hz;
        if (options.pipeline != "public") {
            auto &pull = engine_sim::RuntimeTestAccess::pull(*raw);
            auto *source = &engine_sim::ClockTestAccess::source(pull);
            if (options.pipeline == "profile") { m_probe = std::make_unique<SourceProbe>(*source, stages); source = m_probe.get(); }
            const auto channels = engine_sim::RuntimeTestAccess::simulator(*raw).getEngine()->getExhaustSystemCount();
            m_core = std::make_unique<engine_sim::SampleClock>(m_frequency, std::vector<double>(channels, 0), *source,
                engine_sim::SampleClock::Profile::Public, options.rate);
        }
    }
    void warm() {
        const unsigned end = m_options.rate / 100 * m_options.warmBlocks;
        const std::array<unsigned, 3> events{0, m_options.rate / 4, m_options.rate * 5 / 4};
        std::array<float, 1024> scratch;
        unsigned position = 0;
        for (unsigned phase = 0; phase < events.size(); ++phase) {
            const unsigned stop = phase + 1 < events.size() ? std::min(end, events[phase + 1]) : end;
            es_error error{}; const es_controls value{sizeof(value), ES_ABI_V2, phase != 0, phase == 1,
                m_options.schedule == "legacy" ? .1 : .3, 0, 0};
            checked(es_set_controls(m_handle.get(), &value, &error), error);
            while (position < stop) { const auto count = std::min<unsigned>(scratch.size(), stop - position); pull(scratch.data(), count); m_previousOutput = scratch[count - 1]; position += count; }
        }
        if (position != end) throw std::runtime_error("Warmup control timeline was incomplete");
        stats.before = counts();
    }
    void measure() {
        stages.enabled = true; const auto start = Clock::now();
        {
            CountScope count(stats.renderCounts);
            for (unsigned position = 0; position < m_output.size();) {
                const auto frames = std::min<unsigned>(m_options.block, static_cast<unsigned>(m_output.size()) - position);
                pull(m_output.data() + position, frames); position += frames;
            }
        }
        stats.render = elapsed(start); stages.enabled = false; stats.after = counts();
        const auto total = std::uint64_t(m_options.rate / 100) * (m_options.warmBlocks + m_options.measureBlocks);
        if (stats.after.frames != total || stats.after.nativeFrames != (total * 44100 + m_options.rate - 1) / m_options.rate ||
            stats.after.ticks != total * m_frequency / m_options.rate) throw std::runtime_error("Measured runtime counters differ from rational duration");
        const auto &simulator = engine_sim::RuntimeTestAccess::simulator(*m_handle);
        const auto queues = engine_sim::RuntimeTestAccess::synthesis(*m_handle).getBufferStatistics();
        if (queues.droppedInput || queues.droppedOutput || queues.zeroFilledOutput || queues.stagedInput || queues.pendingInput || queues.activeInput || queues.queuedOutput)
            throw std::runtime_error("Benchmark used, padded or dropped queued audio");
        stats.speed = simulator.getEngine()->getOutputCrankshaft()->m_body.v_theta;
        auto &system = engine_sim::RuntimeTestAccess::system(*m_handle);
        stats.solverAverages = {system.getOdeSolveMicroseconds() * 1e-6, system.getConstraintSolveMicroseconds() * 1e-6,
            system.getForceEvalMicroseconds() * 1e-6, system.getConstraintEvalMicroseconds() * 1e-6};
        for (double value : stats.solverAverages) if (!std::isfinite(value) || value < 0) throw std::runtime_error("Invalid existing solver timing");
        if (!std::isfinite(stats.speed)) throw std::runtime_error("Nonfinite benchmark shaft velocity");
        for (float value : m_output) {
            if (!std::isfinite(value) || value < -1 || value > 32767.0f / 32768) throw std::runtime_error("Invalid benchmark PCM");
            stats.nonzero += value != 0;
        }
        if (!stats.nonzero) throw std::runtime_error("Measured benchmark interval contains no engine signal");
    }
    void save(const fs::path &path) const {
        if (fs::exists(path)) throw std::runtime_error("PCM output already exists");
        std::ofstream stream(path, std::ios::binary); stream.write(reinterpret_cast<const char *>(m_output.data()), static_cast<std::streamsize>(m_output.size() * sizeof(float))); stream.close();
        if (!stream) throw std::runtime_error("Cannot write benchmark PCM");
        auto previous = path; previous += ".previous";
        if (fs::exists(previous)) throw std::runtime_error("Previous-output evidence already exists");
        std::ofstream prefix(previous, std::ios::binary); prefix.write(reinterpret_cast<const char *>(&m_previousOutput), sizeof(m_previousOutput)); prefix.close();
        if (!prefix) throw std::runtime_error("Cannot write previous-output evidence");
    }
    Stats stats;
    Stages stages;
private:
    void pull(float *output, unsigned frames) {
        if (m_core) m_core->render(output, frames);
        else {
            es_error error{}; std::uint32_t written = 99;
            checked(es_render(m_handle.get(), frames, output, frames, &written, &error), error);
            if (written != frames) throw std::runtime_error("Benchmark received short successful PCM");
        }
    }
    engine_sim::ClockCounts counts() const {
        if (m_core) return m_core->counts();
        es_error error{}; es_telemetry value{}; value.size = sizeof(value); value.abi = ES_ABI_V2;
        checked(es_get_telemetry(m_handle.get(), &value, &error), error); return {value.frames, value.ticks, value.native_frames};
    }
    const Options &m_options;
    // Reverse destruction keeps the source and model alive through clock teardown.
    Handle m_handle;
    std::unique_ptr<SourceProbe> m_probe;
    std::unique_ptr<engine_sim::SampleClock> m_core;
    std::vector<float> m_output;
    unsigned m_frequency = 0;
    float m_previousOutput = 0;
};
void ready() {
    std::cout << "READY\n" << std::flush; std::string command;
    if (!std::getline(std::cin, command) || command != "GO") throw std::runtime_error("Benchmark requires GO after READY");
}
int convert(const Options &config) {
    if (config.instances != 1) throw std::invalid_argument("Conversion probe uses one stream");
    const auto count = config.rate / 100 * config.measureBlocks;
    const auto native = 441U * config.measureBlocks;
    if (fs::file_size(config.entry) != std::uint64_t(native + 1) * sizeof(float)) throw std::invalid_argument("Converter needs preceding plus half-open native frames");
    std::vector<float> input(native + 1), output(count); std::vector<std::int16_t> source(native + 1);
    std::ifstream file(config.entry, std::ios::binary); file.read(reinterpret_cast<char *>(input.data()), static_cast<std::streamsize>(input.size() * sizeof(float)));
    if (!file) throw std::runtime_error("Cannot read converter source");
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto value = input[i] * 32768;
        if (!std::isfinite(value) || value < -32768 || value > 32767 || std::floor(value) != value) throw std::invalid_argument("Converter source is not normalized native S16");
        source[i] = static_cast<std::int16_t>(value);
    }
    ready(); const auto before = usage(); const auto start = Clock::now();
    for (unsigned n = 0; n < count; ++n) {
        if (config.rate == 44100) output[n] = static_cast<float>(source[n + 1]) / 32768.0f;
        else {
            const auto position = std::uint64_t(n) * 147; const auto index = position / 160;
            const double fraction = static_cast<double>(position % 160) / 160;
            output[n] = static_cast<float>(((1 - fraction) * source[index] + fraction * source[index + 1]) / 32768.0);
        }
    }
    const auto seconds = elapsed(start); const auto after = usage();
    std::ofstream result(config.output, std::ios::binary); result.imbue(std::locale::classic()); result << std::setprecision(17)
        << "version,1\npipeline,convert\nrate," << config.rate << "\nframes," << count << "\nconversion_seconds," << seconds
        << "\nprocess_cpu_seconds," << after.cpu - before.cpu << "\nrss_before_bytes," << before.current << "\nrss_after_bytes," << after.current << "\npeak_rss_bytes," << after.peak << '\n';
    result.close(); auto target = config.output; target += ".0.f32le";
    if (fs::exists(target)) throw std::runtime_error("Converter output already exists");
    std::ofstream pcm(target, std::ios::binary); pcm.write(reinterpret_cast<const char *>(output.data()), static_cast<std::streamsize>(output.size() * sizeof(float))); pcm.close();
    if (!result || !pcm) throw std::runtime_error("Cannot write converter measurements");
    return 0;
}
int cycles(const Options &config) {
    if (config.instances != 1) throw std::invalid_argument("Lifecycle probe uses one owner");
    es_config settings{}; es_error error{}; checked(es_config_init(ES_ABI_V2, sizeof(settings), &settings, &error), error);
    settings.output_hz = config.rate; settings.seed = config.seed;
    const auto entry = config.entry.u8string(), assets = config.assets.u8string();
    std::array<float, 257> output{};
    auto cycle = [&] {
        es_runtime *raw = nullptr; checked(es_create(&settings, &raw, &error), error); Handle handle(raw, es_destroy);
        checked(es_load_json_file(raw, entry.c_str(), assets.c_str(), nullptr, &error), error);
        const es_controls controls{sizeof(controls), ES_ABI_V2, 1, 1, .3, 0, 0}; checked(es_set_controls(raw, &controls, &error), error);
        std::uint32_t written = 99; checked(es_render(raw, output.size(), output.data(), output.size(), &written, &error), error);
        if (written != output.size()) throw std::runtime_error("Lifecycle measurement lost frames");
        checked(es_unload(raw, &error), error);
    };
    for (unsigned i = 0; i < 3; ++i) cycle();
    ready(); const auto before = usage(); const auto start = Clock::now(); std::array<Usage, 100> samples;
    Counts counts;
    for (auto &sample : samples) { { CountScope count(counts); cycle(); } sample = usage(); }
    const auto seconds = elapsed(start); const auto after = usage();
    std::ofstream result(config.output, std::ios::binary); result.imbue(std::locale::classic()); result << std::setprecision(17)
        << "version,1\npipeline,cycles\ncycles,100\nframes_per_cycle,257\nwall_seconds," << seconds << "\nprocess_cpu_seconds," << after.cpu - before.cpu
        << "\nrss_before_bytes," << before.current << "\nrss_after_bytes," << after.current << "\npeak_rss_bytes," << after.peak
        << "\nallocations," << counts.allocations << "\nallocated_bytes," << counts.bytes << "\nlocks," << counts.locks << "\nprobe," << ES_BENCH_PROBE << '\n';
    for (unsigned i = 0; i < samples.size(); ++i) result << i << ".rss_bytes," << samples[i].current << '\n' << i << ".peak_rss_bytes," << samples[i].peak << '\n';
    result.close(); if (!result) throw std::runtime_error("Cannot write lifecycle measurements"); return 0;
}
int execute(const std::vector<std::string> &args) {
    const auto config = options(args);
    if (config.pipeline == "convert") return convert(config);
    if (config.pipeline == "cycles") return cycles(config);
    double clockPairSeconds = 0;
#if ES_BENCH_PROBE
    // Diagnostic only: estimate the intrusion of each start/end timestamp pair.
    // Keep its raw average separate from stage totals; never subtract noise.
    for (unsigned i = 0; i < 10000; ++i) { const auto start = Clock::now(); clockPairSeconds += elapsed(start); }
    clockPairSeconds /= 10000;
#endif
    std::vector<std::unique_ptr<Instance>> engines;
    for (unsigned i = 0; i < config.instances; ++i) engines.push_back(std::make_unique<Instance>(config));
    std::mutex mutex; std::condition_variable change; unsigned ready = 0; bool go = false, abort = false;
    std::vector<std::exception_ptr> failures(config.instances); std::vector<std::thread> workers;
    struct Join {
        std::vector<std::thread> &threads; std::mutex &mutex; std::condition_variable &change; bool &go; bool &abort;
        ~Join() { { std::lock_guard<std::mutex> lock(mutex); abort = true; go = true; } change.notify_all(); for (auto &thread : threads) if (thread.joinable()) thread.join(); }
    } join{workers, mutex, change, go, abort};
    for (unsigned i = 0; i < config.instances; ++i) workers.emplace_back([&, i] {
        bool announced = false;
        try {
            engines[i]->warm();
            { std::unique_lock<std::mutex> lock(mutex); ++ready; announced = true; change.notify_all(); change.wait(lock, [&] { return go; }); if (abort) return; }
            engines[i]->measure();
        }
        catch (...) { std::lock_guard<std::mutex> lock(mutex); failures[i] = std::current_exception(); if (!announced) ++ready; change.notify_all(); }
    });
    { std::unique_lock<std::mutex> lock(mutex);
        if (!change.wait_for(lock, std::chrono::seconds(120), [&] { return ready == config.instances; })) throw std::runtime_error("Benchmark warmup rendezvous timed out");
        for (const auto &failure : failures) if (failure) std::rethrow_exception(failure);
    }
    bench::ready();
    const auto before = usage(); const auto start = Clock::now();
    { std::lock_guard<std::mutex> lock(mutex); go = true; } change.notify_all();
    for (auto &worker : workers) worker.join();
    const auto seconds = elapsed(start); const auto after = usage();
    for (const auto &failure : failures) if (failure) std::rethrow_exception(failure);
    std::ofstream output(config.output, std::ios::binary); output.imbue(std::locale::classic()); output << std::setprecision(17);
    output << "version,1\npipeline," << config.pipeline << "\nrate," << config.rate << "\nblock," << config.block
        << "\ninstances," << config.instances << "\nwarm_frames," << config.rate / 100 * config.warmBlocks
        << "\nmeasure_frames," << config.rate / 100 * config.measureBlocks << "\nprobe," << ES_BENCH_PROBE
        << "\nmutex_wrap," << ES_BENCH_MUTEX_WRAP << "\nprocess_wall_seconds," << seconds << "\nprocess_cpu_seconds," << after.cpu - before.cpu
        << "\nclock_pair_average_seconds," << clockPairSeconds
        << "\nrss_before_bytes," << before.current << "\nrss_after_bytes," << after.current << "\npeak_rss_bytes," << after.peak << '\n';
    for (unsigned i = 0; i < config.instances; ++i) {
        const auto prefix = std::to_string(i) + '.'; const auto &stats = engines[i]->stats; const auto &stages = engines[i]->stages;
        for (const auto &value : std::array<std::pair<const char *, double>, 8>{{{"create_seconds", stats.create}, {"load_seconds", stats.load}, {"reset_seconds", stats.reset}, {"reload_seconds", stats.reload}, {"render_seconds", stats.render}, {"physics_seconds", stages.physics}, {"native_dsp_seconds", stages.synthesis}, {"latch_seconds", stages.latch}}})
            output << prefix << value.first << ',' << value.second << '\n';
        output << prefix << "frames," << stats.after.frames << '\n' << prefix << "ticks," << stats.after.ticks << '\n'
            << prefix << "native_frames," << stats.after.nativeFrames << '\n' << prefix << "measure_ticks," << stats.after.ticks - stats.before.ticks << '\n'
            << prefix << "measure_native_frames," << stats.after.nativeFrames - stats.before.nativeFrames << '\n'
            << prefix << "crank_rad_s," << stats.speed << '\n' << prefix << "nonzero_frames," << stats.nonzero << '\n'
            << prefix << "profile_ticks," << stages.ticks << '\n' << prefix << "profile_native_frames," << stages.native << '\n';
        const std::array<const char *, 4> solverNames{{"solver_ode_average_seconds", "solver_constraint_average_seconds", "solver_force_average_seconds", "solver_evaluation_average_seconds"}};
        for (unsigned stage = 0; stage < solverNames.size(); ++stage) output << prefix << solverNames[stage] << ',' << stats.solverAverages[stage] << '\n';
        const std::array<std::pair<const char *, Counts>, 5> allocations{{{"create", stats.createCounts}, {"load", stats.loadCounts}, {"reset", stats.resetCounts}, {"reload", stats.reloadCounts}, {"render", stats.renderCounts}}};
        for (const auto &value : allocations) output << prefix << value.first << "_allocations," << value.second.allocations << '\n'
            << prefix << value.first << "_allocated_bytes," << value.second.bytes << '\n' << prefix << value.first << "_locks," << value.second.locks << '\n';
        auto path = config.output; path += "." + std::to_string(i) + ".f32le"; engines[i]->save(path);
    }
    output.close(); if (!output) throw std::runtime_error("Cannot write benchmark measurements");
    return 0;
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    try { std::vector<std::string> args; for (int i = 0; i < argc; ++i) args.push_back(std::filesystem::path(argv[i]).u8string()); return bench::execute(args); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
#else
int main(int argc, char **argv) {
    try { return bench::execute(std::vector<std::string>(argv, argv + argc)); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
#endif
