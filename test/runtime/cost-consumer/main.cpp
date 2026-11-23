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

#include <engine_sim/runtime.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Handle = std::unique_ptr<es_runtime, decltype(&es_destroy)>;
constexpr unsigned warm_steps = 200;
constexpr unsigned first_seed = 403;
constexpr std::array<unsigned, 8> irregular{1, 257, 13, 1024, 3, 511, 37, 49};

double elapsed(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

struct Usage {
    double cpu_seconds;
    std::int64_t current_bytes;
    std::uint64_t peak_bytes;
};

Usage usage() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) ||
        !GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
        throw std::runtime_error("Cannot read process CPU time and working set");
    }
    const auto seconds = [](FILETIME time) {
        ULARGE_INTEGER bits;
        bits.LowPart = time.dwLowDateTime;
        bits.HighPart = time.dwHighDateTime;
        return static_cast<double>(bits.QuadPart) * 1e-7;
    };
    return {seconds(kernel) + seconds(user), static_cast<std::int64_t>(memory.WorkingSetSize),
            memory.PeakWorkingSetSize};
#else
    rusage data{};
    if (getrusage(RUSAGE_SELF, &data) != 0) {
        throw std::runtime_error("Cannot read process CPU time and resident memory");
    }
    const double cpu = data.ru_utime.tv_sec + data.ru_utime.tv_usec * 1e-6
                     + data.ru_stime.tv_sec + data.ru_stime.tv_usec * 1e-6;
#ifdef __linux__
    std::ifstream statm("/proc/self/statm");
    std::int64_t virtual_pages, resident_pages;
    const auto page = sysconf(_SC_PAGESIZE);
    if (!(statm >> virtual_pages >> resident_pages) || page <= 0) {
        throw std::runtime_error("Cannot read current process RSS");
    }
    return {cpu, resident_pages * page, static_cast<std::uint64_t>(data.ru_maxrss) * 1024};
#elif defined(__APPLE__)
    return {cpu, -1, static_cast<std::uint64_t>(data.ru_maxrss)};
#else
#error Specify ru_maxrss units for this platform before using the cost consumer.
#endif
#endif
}

void check(es_status status, const es_error &error) {
    if (status != ES_OK) throw std::runtime_error(error.message);
}

unsigned number(const std::string &argument) {
    std::size_t digits = 0;
    const auto result = std::stoul(argument, &digits);
    if (digits != argument.size() || result > 48000) {
        throw std::invalid_argument("Invalid numeric workload field");
    }
    return static_cast<unsigned>(result);
}

struct Options {
    std::string entry, assets, output;
    unsigned rate, block, owners;
    bool fixture, driven, keep_transients;
};

Options options(const std::vector<std::string> &args) {
    if (args.size() != 10 || (args[7] != "fixture" && args[7] != "legacy") ||
        (args[8] != "free" && args[8] != "driven") ||
        (args[9] != "steady" && args[9] != "transients")) {
        throw std::invalid_argument("Expected model asset-root output rate block owners fixture|legacy free|driven steady|transients");
    }
    Options result{args[1], args[2], args[3], number(args[4]), number(args[5]), number(args[6]),
                   args[7] == "fixture", args[8] == "driven", args[9] == "transients"};
    if ((result.rate != 44100 && result.rate != 48000) ||
        (result.block != 0 && result.block != 1 && result.block != 256 && result.block != 1024) ||
        result.owners == 0 || result.owners > 2) {
        throw std::invalid_argument("Unsupported rate, block or owner count");
    }
    return result;
}

class Owner {
public:
    Owner(const Options &settings, unsigned seed)
        : m_options(settings), m_pcm(settings.rate), m_handle(nullptr, es_destroy) {
        if (settings.keep_transients) m_transients.resize(2 * settings.rate);
        es_error error{};
        es_config config{};
        check(es_config_init(ES_ABI_V2, sizeof(config), &config, &error), error);
        config.output_hz = settings.rate;
        config.mode = settings.driven ? ES_DRIVEN : ES_FREE;
        config.seed = seed;
        es_runtime *raw = nullptr;
        check(es_create(&config, &raw, &error), error);
        m_handle.reset(raw);
        const auto start = Clock::now();
        const auto status = es_load_json_file(raw, settings.entry.c_str(), settings.assets.c_str(), nullptr, &error);
        m_load_seconds = elapsed(start);
        check(status, error);
        validate_clock(0, telemetry());
    }

    void warm() {
        std::array<float, 1024> scratch;
        const unsigned step_frames = m_options.rate / 100;
        for (unsigned step = 0; step < warm_steps; ++step) {
            if (m_options.driven || step == 0 || step == 25 || step == 60 ||
                step == 80 || step == 90 || step == 125 || step == 145) {
                apply_controls(step);
            }
            float *output = m_options.keep_transients ? m_transients.data() + step * step_frames : scratch.data();
            pull(output, step_frames);
            m_previous = output[step_frames - 1];
        }
        m_before = telemetry();
        validate_clock(2 * m_options.rate, m_before);
    }

    void measure() {
        unsigned partition = 0;
        for (unsigned offset = 0; offset < m_options.rate;) {
            const auto block = m_options.block ? m_options.block : irregular[partition++ % irregular.size()];
            const auto count = std::min(block, m_options.rate - offset);
            pull(m_pcm.data() + offset, count);
            offset += count;
        }
        m_after = telemetry();
        validate_clock(3 * m_options.rate, m_after);
        m_nonzero = std::count_if(m_pcm.begin(), m_pcm.end(), [](float sample) {
            if (!std::isfinite(sample) || sample < -1 || sample > 32767.0f / 32768) {
                throw std::runtime_error("Invalid benchmark PCM");
            }
            return sample != 0;
        });
        if (m_nonzero == 0) throw std::runtime_error("Silent benchmark window");
    }

    void report(std::ostream &stream, unsigned index) const {
        const auto prefix = std::to_string(index) + ".";
        stream << prefix << "load_seconds," << m_load_seconds << '\n'
               << prefix << "frames," << m_after.frames << '\n'
               << prefix << "ticks," << m_after.ticks << '\n'
               << prefix << "native_frames," << m_after.native_frames << '\n'
               << prefix << "measure_ticks," << m_after.ticks - m_before.ticks << '\n'
               << prefix << "measure_native_frames," << m_after.native_frames - m_before.native_frames << '\n'
               << prefix << "crank_rad_s," << m_after.crank_rad_s << '\n'
               << prefix << "nonzero_frames," << m_nonzero << '\n';
        const auto path = m_options.output + "." + prefix + "f32le";
        save(path, m_pcm.data(), m_pcm.size());
        save(path + ".previous", &m_previous, 1);
        if (m_options.keep_transients) save(path + ".warm", m_transients.data(), m_transients.size());
    }

private:
    void apply_controls(unsigned step) {
        const unsigned ignition = step >= 25 && !(step >= 80 && step < 90);
        const unsigned starter = !m_options.driven && step >= 25 && step < 125;
        const double demand = m_options.fixture ? (step < 60 ? .30 : .35) : (step < 60 ? .10 : .13);
        const double load = step >= 145 ? (m_options.driven ? 3.0 : .5) : 0;
        double rpm = 0;
        if (m_options.driven && step >= 10) {
            rpm = step < 40 ? 600 : step < 110 ? 600 + (step - 40) * 1200.0 / 70.0 : 1800;
        }
        const es_controls controls{sizeof(controls), ES_ABI_V2, ignition, starter, demand, load, rpm};
        es_error error{};
        check(es_set_controls(m_handle.get(), &controls, &error), error);
    }

    void pull(float *output, unsigned frames) {
        es_error error{};
        std::uint32_t written = 0;
        check(es_render(m_handle.get(), frames, output, frames, &written, &error), error);
        if (written != frames) throw std::runtime_error("Short successful render");
    }

    es_telemetry telemetry() const {
        es_telemetry value{};
        value.size = sizeof(value);
        value.abi = ES_ABI_V2;
        es_error error{};
        check(es_get_telemetry(m_handle.get(), &value, &error), error);
        return value;
    }

    void validate_clock(std::uint64_t frames, const es_telemetry &value) const {
        const auto rate = m_options.rate;
        if (value.state != ES_READY || value.output_hz != rate || !value.simulation_hz ||
            value.mode != (m_options.driven ? ES_DRIVEN : ES_FREE) || !std::isfinite(value.crank_rad_s) ||
            value.frames != frames || value.ticks != frames * value.simulation_hz / rate ||
            value.native_frames != (frames * 44100 + rate - 1) / rate) {
            throw std::runtime_error("Measured runtime clock differs from requested duration");
        }
    }

    static void save(const std::string &path, const float *samples, std::size_t count) {
        const auto target = std::filesystem::u8path(path);
        if (std::filesystem::exists(target)) throw std::runtime_error("PCM output already exists");
        std::ofstream file(target, std::ios::binary);
        file.write(reinterpret_cast<const char *>(samples), static_cast<std::streamsize>(count * sizeof(float)));
        file.close();
        if (!file) throw std::runtime_error("Cannot write PCM");
    }

    const Options &m_options;
    std::vector<float> m_pcm, m_transients;
    Handle m_handle;
    double m_load_seconds = 0;
    float m_previous = 0;
    es_telemetry m_before{}, m_after{};
    std::size_t m_nonzero = 0;
};

int run(const std::vector<std::string> &args) {
    const auto settings = options(args);
    const auto path = std::filesystem::u8path(settings.output);
    if (std::filesystem::exists(path)) throw std::runtime_error("Measurement output already exists");

    std::vector<std::unique_ptr<Owner>> owners;
    for (unsigned index = 0; index < settings.owners; ++index) {
        owners.push_back(std::make_unique<Owner>(settings, first_seed + index));
    }
    std::mutex mutex;
    std::condition_variable change;
    unsigned ready = 0;
    bool go = false, abort = false;
    std::vector<std::exception_ptr> failures(settings.owners);
    std::vector<std::thread> workers;
    struct JoinWorkers {
        std::vector<std::thread> &workers;
        std::mutex &mutex;
        std::condition_variable &change;
        bool &go, &abort;
        ~JoinWorkers() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                abort = go = true;
            }
            change.notify_all();
            for (auto &worker : workers) {
                if (worker.joinable()) worker.join();
            }
        }
    } join{workers, mutex, change, go, abort};

    for (unsigned index = 0; index < settings.owners; ++index) {
        workers.emplace_back([&, index] {
            bool announced = false;
            try {
                owners[index]->warm();
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    ++ready;
                    announced = true;
                    change.notify_all();
                    change.wait(lock, [&] { return go; });
                    if (abort) return;
                }
                owners[index]->measure();
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                failures[index] = std::current_exception();
                if (!announced) ++ready;
                change.notify_all();
            }
        });
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!change.wait_for(lock, std::chrono::seconds(120), [&] { return ready == settings.owners; })) {
            throw std::runtime_error("Warmup rendezvous timed out");
        }
        for (const auto &failure : failures) if (failure) std::rethrow_exception(failure);
    }
    std::cout << "READY\n" << std::flush;
    std::string command;
    if (!std::getline(std::cin, command) || command != "GO") {
        throw std::runtime_error("Expected GO after READY");
    }
    const auto before = usage();
    const auto start = Clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex);
        go = true;
    }
    change.notify_all();
    for (auto &worker : workers) worker.join();
    const auto wall = elapsed(start);
    const auto after = usage();
    for (const auto &failure : failures) if (failure) std::rethrow_exception(failure);

    std::ofstream output(path, std::ios::binary);
    output.imbue(std::locale::classic());
    output << std::setprecision(17)
           << "version,1\npipeline,installed\nrate," << settings.rate << "\nblock," << settings.block
           << "\ninstances," << settings.owners << "\nwarm_frames," << 2 * settings.rate
           << "\nmeasure_frames," << settings.rate << "\nprocess_wall_seconds," << wall
           << "\nprocess_cpu_seconds," << after.cpu_seconds - before.cpu_seconds
           << "\nrss_before_bytes," << before.current_bytes << "\nrss_after_bytes," << after.current_bytes
           << "\npeak_rss_bytes," << after.peak_bytes << '\n';
    for (unsigned index = 0; index < settings.owners; ++index) owners[index]->report(output, index);
    output.close();
    if (!output) throw std::runtime_error("Cannot write measurement rows");
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
