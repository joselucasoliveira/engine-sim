#ifndef ENGINE_SIM_PRIVATE_FLEET_BENCH_TRACE_HPP
#define ENGINE_SIM_PRIVATE_FLEET_BENCH_TRACE_HPP

#include <charconv>
#include <cfenv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#endif

namespace engine_sim::fleet {

class BenchTrace {
public:
    using Clock = std::chrono::steady_clock;
    struct Tile {
        std::uint64_t first, submitted, begin, published, end;
    };
    struct IdlePoll {
        std::uint64_t begin, end, generated, submittedBefore, submittedAfter;
    };

    BenchTrace() {
        const auto *location = std::getenv("ES_FLEET_TRACE_FILE");
        if (!location || !*location) return;
        const auto *capacity = std::getenv("ES_FLEET_TRACE_TILES");
        if (!capacity || !*capacity) throw std::runtime_error("Missing producer trace capacity");
        path = location;
        std::size_t count = 0;
        const auto limit = capacity + std::char_traits<char>::length(capacity);
        const auto parsed = std::from_chars(capacity, limit, count);
        if (parsed.ec != std::errc{} || parsed.ptr != limit || count == 0 || count > 2'000'000)
            throw std::runtime_error("Invalid producer trace capacity");
        tiles.resize(count);
        waits.resize(65'536);
        roundBefore = std::fegetround();
#if defined(__linux__)
        if (const auto *core = std::getenv("ES_FLEET_WORKER_CORE")) {
            int cpu = -1;
            const auto end = core + std::char_traits<char>::length(core);
            const auto number = std::from_chars(core, end, cpu);
            if (number.ec != std::errc{} || number.ptr != end || cpu < 0 || cpu >= CPU_SETSIZE)
                throw std::runtime_error("Invalid producer core");
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(cpu, &set);
            if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
                throw std::runtime_error("Cannot set producer core affinity");
        }
#endif
    }

    static std::uint64_t now() noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
    }

    std::uint64_t begin() const noexcept { return path.empty() ? 0 : now(); }
    void startTile(std::uint64_t first, std::uint64_t submitted) noexcept {
        inFlight = {first, submitted, begin(), 0, 0};
    }
    void finishTile(std::uint64_t first, std::uint64_t published) noexcept {
        if (inFlight.begin && count < tiles.size()) {
            inFlight.first = first;
            inFlight.published = published;
            inFlight.end = now();
            tiles[count++] = inFlight;
        }
        inFlight.begin = 0;
    }
    void idle(std::uint64_t start, std::uint64_t end, std::uint64_t generated,
              std::uint64_t before, std::uint64_t after) noexcept {
        if (start == 0) return;
        ++idlePolls;
        const auto elapsed = end - start;
        if (elapsed > longestWait) longestWait = elapsed;
        if (elapsed >= 200'000) waits[slowPolls++ % waits.size()] = {start, end, generated, before, after};
    }

    ~BenchTrace() {
        if (path.empty()) return;
        const auto closed = now();
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(tiles.data()), static_cast<std::streamsize>(count * sizeof(Tile)));
        std::ofstream conditions(path + ".tsv");
        conditions << "worker_rounding_before " << roundBefore << "\nworker_rounding_after " << std::fegetround() << '\n';
        conditions << "worker_idle_polls " << idlePolls << "\nworker_slow_idle_polls " << slowPolls
                   << "\nworker_max_idle_poll_ns " << longestWait << '\n';
        if (inFlight.begin) conditions << "unpublished_tile " << inFlight.first << "\nunpublished_started_ns "
                                       << inFlight.begin << "\nunpublished_closed_ns " << closed << '\n';
        std::ofstream idleTrace(path + ".idle.tsv");
        idleTrace << "begin_ns\tend_ns\tG\tD_before\tD_after\n";
        const auto first = slowPolls > waits.size() ? slowPolls - waits.size() : 0;
        for (auto i = first; i < slowPolls; ++i) {
            const auto &wait = waits[i % waits.size()];
            idleTrace << wait.begin << '\t' << wait.end << '\t' << wait.generated << '\t'
                      << wait.submittedBefore << '\t' << wait.submittedAfter << '\n';
        }
#if defined(__linux__)
        rusage usage{};
        if (getrusage(RUSAGE_THREAD, &usage) == 0) {
            conditions << "worker_cpu_ns " << (static_cast<std::uint64_t>(usage.ru_utime.tv_sec) + usage.ru_stime.tv_sec) *
                UINT64_C(1000000000) + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * UINT64_C(1000)
                << "\nworker_cpu_at_teardown " << sched_getcpu() << "\nworker_voluntary_switches " << usage.ru_nvcsw
                << "\nworker_involuntary_switches " << usage.ru_nivcsw << '\n';
        }
#endif
    }

private:
    std::string path;
    std::vector<Tile> tiles;
    std::vector<IdlePoll> waits;
    Tile inFlight{};
    std::size_t count = 0;
    std::size_t idlePolls = 0, slowPolls = 0;
    std::uint64_t longestWait = 0;
    int roundBefore = -1;
};

}
#endif
