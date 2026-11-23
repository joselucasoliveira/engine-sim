#include "engine_sim/runtime.h"
#include "observe.hpp"

#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
using Handle = std::unique_ptr<es_runtime, decltype(&es_destroy)>;
namespace fs = std::filesystem;

void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

void checked(es_status status, const es_error &error) {
    if (status != ES_OK) throw std::runtime_error(error.message);
}

es_telemetry telemetry(es_runtime *runtime) {
    es_error error{};
    es_telemetry result{};
    result.size = sizeof(result);
    result.abi = ES_ABI_V2;
    const auto status = es_get_telemetry(runtime, &result, &error);
    checked(status, error);
    return result;
}

void load(es_runtime *runtime, const fs::path &entry, const fs::path &assets) {
    es_error error{};
    const auto status = es_load_json_file(runtime, entry.c_str(), assets.c_str(), nullptr, &error);
    checked(status, error);
}

Handle prepare(const fs::path &entry, const fs::path &assets, unsigned rate, bool driven) {
    es_config config{};
    es_error error{};
    checked(es_config_init(ES_ABI_V2, sizeof(config), &config, &error), error);
    config.output_hz = rate;
    config.mode = driven ? ES_DRIVEN : ES_FREE;
    config.seed = 403;
    es_runtime *runtime = nullptr;
    checked(es_create(&config, &runtime, &error), error);
    Handle owner(runtime, es_destroy);
    load(runtime, entry, assets);
    return owner;
}

void controls(es_runtime *runtime, bool driven, bool saturation = false, double rpm = 0) {
    es_error error{};
    const es_controls input{sizeof(input), ES_ABI_V2, 1, driven ? 0u : 1u,
        saturation ? .2 : .25, saturation ? 100. : 0., rpm};
    checked(es_set_controls(runtime, &input, &error), error);
}

template<class Work>
void on_owner(Work work) {
    const auto preparer = std::this_thread::get_id();
    std::exception_ptr failure;
    std::thread thread([&] {
        try {
            const auto owner = std::this_thread::get_id();
            require(owner != preparer, "render must use a different owner");
            std::printf("preparer %zu owner %zu rounding %d\n", std::hash<std::thread::id>{}(preparer),
                        std::hash<std::thread::id>{}(owner), std::fegetround());
            work();
        } catch (...) {
            failure = std::current_exception();
        }
    });
    thread.join();
    if (failure) std::rethrow_exception(failure);
}

template<std::size_t N>
struct Pull {
    std::array<float, N + 2> pcm{};
    es_telemetry snapshot{};
    std::uint32_t written = 0;
    es_status status = ES_OK;
};

template<std::size_t N>
Pull<N> render(es_runtime *runtime, const char *phase) {
    Pull<N> result;
    result.pcm.fill(42.f);
    es_error error{};
    realtime_probe::Record hits{};
    {
        const realtime_probe::Scope measured(hits);
        result.status = es_render(runtime, N, result.pcm.data() + 1, N, &result.written, &error);
    }
    const auto &t = result.snapshot = telemetry(runtime);
    std::printf("phase %s\n", phase);
    realtime_probe::dump(hits);
    require(result.written <= N, "invalid render count");
    require(result.pcm.front() == 42.f, "render wrote before output");
    const auto end = result.pcm.begin() + 1 + result.written;
    require(std::all_of(end, result.pcm.end(), [](float x) { return x == 42.f; }), "render touched suffix");
    require(std::all_of(result.pcm.begin() + 1, end, [](float x) { return std::isfinite(x); }), "nonfinite PCM");
    std::printf("snapshot %s %d %u %u %u %u %u %u %llu %llu %llu"
                " %a %a %a %a %a %a %a %a %a %a\n", phase, result.status, result.written, t.state, t.mode,
                t.output_hz, t.simulation_hz, t.cylinders, static_cast<unsigned long long>(t.frames),
                static_cast<unsigned long long>(t.ticks), static_cast<unsigned long long>(t.native_frames),
                t.measured_rpm, t.crank_rad_s, t.cycle_rad, t.plate_angle_rad, t.requested_load_nm,
                t.load_reaction_nm, t.drive_reaction_nm, t.applied_driven_rpm, t.driven_limit_rpm,
                t.displacement_m3);
    std::printf("pcm %s", phase);
    for (auto it = result.pcm.begin() + 1; it != end; ++it) {
        std::uint32_t bits;
        static_assert(sizeof(bits) == sizeof(*it));
        std::memcpy(&bits, &*it, sizeof(bits));
        std::printf(" %08x", bits);
    }
    std::printf("\n");
    std::fflush(stdout);
    if (result.status != ES_OK && result.status != ES_RENDER_FAULT) checked(result.status, error);
    return result;
}

template<std::size_t N>
void full(const Pull<N> &pull, std::uint64_t frames) {
    require(pull.status == ES_OK && pull.written == N && pull.snapshot.frames == frames, "short or incorrect full render");
    require(pull.snapshot.ticks == frames * pull.snapshot.simulation_hz / pull.snapshot.output_hz, "incorrect tick count");
}

void check_fresh(es_runtime *runtime) {
    const auto t = telemetry(runtime);
    require(t.state == ES_READY && t.frames == 0 && t.ticks == 0 && t.native_frames == 0, "pre-rendered model");
    std::printf("startup frames=%llu ticks=%llu native=%llu\n",
                static_cast<unsigned long long>(t.frames), static_cast<unsigned long long>(t.ticks),
                static_cast<unsigned long long>(t.native_frames));
}

void fixture(const fs::path &source, unsigned rate, bool driven) {
    const auto root = source / "examples/runtime/fixture";
    const auto entry = root / "main.json";
    auto runtime = prepare(entry, root, rate, driven);
    controls(runtime.get(), driven);
    Pull<256> start;
    on_owner([&] {
        check_fresh(runtime.get());
        start = render<256>(runtime.get(), "initial");
        full(start, 256);
        const auto steady = render<256>(runtime.get(), "next");
        full(steady, 512);
    });
    es_error error{};
    checked(es_reset(runtime.get(), &error), error);
    controls(runtime.get(), driven);
    const auto compare_start = [&](const char *phase) {
        on_owner([&] {
            check_fresh(runtime.get());
            const auto pulled = render<256>(runtime.get(), phase);
            full(pulled, 256);
            require(std::memcmp(pulled.pcm.data(), start.pcm.data(), sizeof(start.pcm)) == 0, "first PCM changed");
            require(std::memcmp(&pulled.snapshot, &start.snapshot, sizeof(start.snapshot)) == 0, "first telemetry changed");
        });
    };
    compare_start("after_reset");
    load(runtime.get(), entry, root);
    controls(runtime.get(), driven);
    compare_start("after_replacement");

    auto split = prepare(entry, root, rate, driven);
    controls(split.get(), driven);
    on_owner([&] {
        check_fresh(split.get());
        for (unsigned i = 1; i <= 256; ++i) {
            const auto pulled = render<1>(split.get(), i == 1 ? "first_frame" : "toward_first_tick");
            full(pulled, i);
            require(std::memcmp(&pulled.pcm[1], &start.pcm[i], sizeof(float)) == 0, "one-frame partition changed PCM");
            if (pulled.snapshot.ticks != 0) {
                std::printf("first_tick frame=%u ticks=%llu\n", i, static_cast<unsigned long long>(pulled.snapshot.ticks));
                return;
            }
        }
        throw std::runtime_error("no tick in initial block");
    });
}

void fault(const fs::path &source) {
    const auto entry = source / "assets/engines/atg-video-1/02_kohler_ch750.json";
    auto runtime = prepare(entry, source, 44100, true);
    auto reference = prepare(entry, source, 44100, true);
    controls(runtime.get(), true, true);
    controls(reference.get(), true, true);
    on_owner([&] {
        const auto first = render<2205>(runtime.get(), "fault_setup");
        const auto same = render<2205>(reference.get(), "fault_control_setup");
        full(first, 2205);
        full(same, 2205);
        require(std::memcmp(first.pcm.data(), same.pcm.data(), sizeof(first.pcm)) == 0, "fault setup mismatch");
        const auto good = render<1>(reference.get(), "fault_control_prefix");
        full(good, 2206);
        controls(runtime.get(), true, true, 3600);
        const auto pulled = render<64>(runtime.get(), "fault_prefix");
        require(pulled.status == ES_RENDER_FAULT && pulled.written == 1 && pulled.snapshot.state == ES_FAULTED,
                "incorrect fault status/prefix");
        require(std::memcmp(&pulled.pcm[1], &good.pcm[1], sizeof(float)) == 0, "good prefix changed");
        auto faulted = pulled.snapshot;
        faulted.state = good.snapshot.state;
        require(std::memcmp(&faulted, &good.snapshot, sizeof(faulted)) == 0, "fault did not preserve last good telemetry");
    });
}
}
int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) return 2;
    try {
        const auto source = fs::absolute(argv[1]);
        if (argc == 3 && std::strcmp(argv[2], "fault") == 0) {
            fault(source);
        } else if (argc == 4) {
            const bool driven = std::strcmp(argv[2], "driven") == 0;
            const unsigned rate = static_cast<unsigned>(std::stoul(argv[3]));
            require(driven || std::strcmp(argv[2], "free") == 0, "unknown mode");
            require(rate == 44100 || rate == 48000, "unknown rate");
            fixture(source, rate, driven);
        } else {
            return 2;
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    return 0;
}
