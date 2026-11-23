#include "capacity.hpp"
#include "json_session.hpp"
#include "piston_engine_simulator.h"
#include "shaft_drive.hpp"

#include <algorithm>
#include <array>
#include <cfenv>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace instrumentation {
struct Observation {
    std::size_t allocations = 0, releases = 0, locks = 0;
    std::size_t fail_at = 0;
    bool poison = false;
};
thread_local Observation *current = nullptr;

class Scope {
public:
    explicit Scope(Observation &value) : previous(std::exchange(current, &value)) {}
    ~Scope() { current = previous; }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
private:
    Observation *previous;
};

void *allocate(std::size_t bytes) {
    if (current && ++current->allocations == current->fail_at) throw std::bad_alloc();
    if (auto *storage = std::malloc(std::max<std::size_t>(bytes, 1))) {
        if (current && current->poison) std::memset(storage, 0x5a, bytes);
        return storage;
    }
    throw std::bad_alloc();
}
void release(void *storage) noexcept {
    if (storage && current) ++current->releases;
    std::free(storage);
}
}

void *operator new(std::size_t bytes) { return instrumentation::allocate(bytes); }
void *operator new[](std::size_t bytes) { return instrumentation::allocate(bytes); }
void operator delete(void *storage) noexcept { instrumentation::release(storage); }
void operator delete[](void *storage) noexcept { instrumentation::release(storage); }
void operator delete(void *storage, std::size_t) noexcept { instrumentation::release(storage); }
void operator delete[](void *storage, std::size_t) noexcept { instrumentation::release(storage); }

#if defined(ES_WORKSPACE_WRAP_LOCK)
#include <pthread.h>
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t *);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t *value) {
    if (instrumentation::current) ++instrumentation::current->locks;
    return __real_pthread_mutex_lock(value);
}
#endif

namespace {
namespace fs = std::filesystem;
namespace json = engine_sim::json_runtime;
namespace cfg = engine_sim::config_v1;
using Owner = std::unique_ptr<es_runtime, decltype(&es_destroy)>;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void check(es_status value, const es_error &error) {
    if (value != ES_OK) throw std::runtime_error(error.message);
}

es_config profile(bool driven, unsigned rate) {
    es_config result{};
    es_error error{};
    check(es_config_init(ES_ABI_V2, sizeof(result), &result, &error), error);
    result.mode = driven ? ES_DRIVEN : ES_FREE;
    result.output_hz = rate;
    result.seed = 403;
    return result;
}

void load(es_runtime *handle, const fs::path &model, const fs::path &assets) {
    es_error error{};
    check(json::load_file(handle, model, assets, nullptr, &error), error);
}

Owner prepare(const fs::path &model, const fs::path &assets, es_config config) {
    es_runtime *result = nullptr;
    es_error error{};
    check(es_create(&config, &result, &error), error);
    Owner owner(result, es_destroy);
    load(result, model, assets);
    return owner;
}

void command(es_runtime *voice, bool driven) {
    const es_controls controls{sizeof(es_controls), ES_ABI_V2, 1, driven ? 0u : 1u, .25, .0, 0};
    es_error error{};
    check(es_set_controls(voice, &controls, &error), error);
}

es_telemetry telemetry(es_runtime *voice) {
    es_telemetry result{};
    result.size = sizeof(result);
    result.abi = ES_ABI_V2;
    es_error error{};
    check(es_get_telemetry(voice, &result, &error), error);
    return result;
}

struct Block {
    std::vector<float> pcm;
    es_telemetry state;
};

Block render(es_runtime *voice, std::uint32_t count, const char *phase) {
    Block block{std::vector<float>(count + 2, 42.f), {}};
    es_error error{};
    std::uint32_t written = 0;
    instrumentation::Observation observed;
    es_status status;
    {
        const instrumentation::Scope guard(observed);
        status = es_render(voice, count, block.pcm.data() + 1, count, &written, &error);
    }
    if (phase) {
        std::cout << phase << " frames=" << count << " allocations=" << observed.allocations
                  << " releases=" << observed.releases << " observed_locks=" << observed.locks
                  << " status=" << status << '\n';
        require(observed.allocations == 0 && observed.releases == 0 && observed.locks == 0,
                "Cold render allocated, freed, or acquired an observed lock");
    }
    check(status, error);
    require(written == count && block.pcm.front() == 42.f && block.pcm.back() == 42.f, "Incorrect render extent");
    block.state = telemetry(voice);
    return block;
}

void compare(const Block &left, const Block &right) {
    require(left.pcm.size() == right.pcm.size() &&
        std::memcmp(left.pcm.data(), right.pcm.data(), left.pcm.size() * sizeof(float)) == 0,
        "PCM differs from fresh isolated voice");
    require(std::memcmp(&left.state, &right.state, sizeof(es_telemetry)) == 0,
            "Telemetry differs from fresh isolated voice");
}

template<class Call> void on_owner(const Call &call) {
    std::exception_ptr failure;
    const auto preparer = std::this_thread::get_id();
    std::thread owner([&] {
        try {
            require(preparer != std::this_thread::get_id(), "Cold validation owner is the preparer");
            require(std::fegetround() == FE_TONEAREST, "Unexpected owner FP rounding");
            call();
        } catch (...) { failure = std::current_exception(); }
    });
    owner.join();
    if (failure) std::rethrow_exception(failure);
}

std::array<Block, 4> first_use(es_runtime *handle) {
    std::array<Block, 4> history;
    on_owner([&] {
        const auto fresh = telemetry(handle);
        require(fresh.state == ES_READY && fresh.frames == 0 && fresh.ticks == 0 && fresh.native_frames == 0,
                "Preparation pre-rendered a frame");
        const auto tick = (fresh.output_hz + fresh.simulation_hz - 1) / fresh.simulation_hz;
        require(tick > 1, "The fixture must separate the first sample and the first physics tick");
        history[0] = render(handle, 1, "first-sample");
        history[1] = render(handle, tick - 1, "first-tick");
        history[2] = render(handle, 256 - tick, "first-256");
        require(history[0].state.ticks == 0 && history[1].state.ticks == 1 && history[2].state.frames == 256,
                "Cold counters do not match the integer schedule");
    });
    on_owner([&] { history[3] = render(handle, 256, "continued-block"); });
    return history;
}

void cold(const fs::path &model, const fs::path &assets) {
    for (bool driven : {false, true}) {
        for (unsigned rate : {44100u, 48000u}) {
            auto voice = prepare(model, assets, profile(driven, rate));
            command(voice.get(), driven);
            std::cout << model.filename().u8string() << " mode=" << (driven ? "DRIVEN" : "FREE") << " rate=" << rate << '\n';
            const auto expected = first_use(voice.get());
            es_error error{};
            for (bool reset : {true, false}) {
                if (reset) check(json::reset(voice.get(), nullptr, &error), error);
                else load(voice.get(), model, assets);
                command(voice.get(), driven);
                const auto actual = first_use(voice.get());
                for (std::size_t i = 0; i < expected.size(); ++i) compare(expected[i], actual[i]);
            }
        }
    }
}

template<class Callback> instrumentation::Observation fail_allocation(std::size_t index, const Callback &call) {
    instrumentation::Observation observation{};
    observation.fail_at = index;
    bool rejected = false;
    try {
        const instrumentation::Scope scope(observation);
        call();
    } catch (const std::bad_alloc &) { rejected = true; }
    require(rejected && observation.allocations == index, "Expected injected allocation failure did not arrive");
    return observation;
}

struct Dense { atg_scs::Matrix value; ~Dense() { value.destroy(); } };
struct Sparse { atg_scs::SparseMatrix<3> value; ~Sparse() { value.destroy(); } };

void rollback_arrays() {
    Dense dense;
    dense.value.initialize(2, 2, 19.0);
    for (std::size_t index : {1, 2}) {
        const auto observation = fail_allocation(index, [&] { dense.value.resize(9, 9); });
        require(observation.releases + 1 == index && dense.value.getWidth() == 2 && dense.value.getHeight() == 2 &&
                dense.value.get(1, 1) == 19.0, "Dense growth failure changed original storage or leaked replacement");
    }
    dense.value.initialize(9, 9, 31.0);
    require(dense.value.get(8, 8) == 31.0, "Dense retry did not initialize its final stride");
    Sparse sparse;
    sparse.value.initialize(6, 2);
    sparse.value.setBlock(1, 0, 1);
    sparse.value.set(1, 0, 2, 23.0);
    for (std::size_t index : {1, 2, 3}) {
        const auto observation = fail_allocation(index, [&] { sparse.value.resize(12, 9); });
        Dense expanded;
        sparse.value.expand(&expanded.value);
        require(observation.releases + 1 == index && expanded.value.getHeight() == 2 &&
                expanded.value.getWidth() == 6 && expanded.value.get(5, 1) == 23.0,
                "Sparse growth failure changed original metadata or leaked replacement");
    }
    sparse.value.destroy();
    sparse.value.initialize(6, 1);
    sparse.value.setEmpty(0, 1);
    require(sparse.value.get(0, 1, 2) == 0.0, "Sparse destroy left unusable capacity");
}

void rollback_graph(const fs::path &model, const fs::path &assets, bool driven) {
    auto opts = profile(driven, 48000);
    auto active = prepare(model, assets, opts);
    auto twin = prepare(model, assets, opts);
    command(active.get(), driven);
    command(twin.get(), driven);
    compare(render(active.get(), 256, nullptr), render(twin.get(), 256, nullptr));
    std::size_t fail_points = 0;
    const auto candidate = [&](std::size_t index) {
        engine_sim::assembly::FleetCapacity capacity;
        auto result = json::prepare_fleet_file(model, assets, opts, capacity);
        if (const auto *problem = std::get_if<cfg::Diagnostic>(&result)) throw std::runtime_error(problem->message);
        auto prepared = std::get<json::PreparedModel>(std::move(result));
        auto &sim = static_cast<PistonEngineSimulator &>(*prepared.session->simulator);
        std::unique_ptr<engine_sim::DrivenShaft> drive;
        if (driven) drive = std::make_unique<engine_sim::DrivenShaft>(sim);
        else sim.enableExternalLoad();
        const auto &shape = capacity.predicted;
        const auto &system = *sim.getSystem();
        require(system.getRigidBodyCount() == static_cast<int>(shape.bodies) &&
            system.getConstraintCount() == static_cast<int>(shape.constraints) &&
            system.getFullConstraintCount() == static_cast<int>(shape.rows) &&
            system.getForceGeneratorCount() == static_cast<int>(shape.generators), "Final solver graph differs from reservations");
        if (index) {
            (void)fail_allocation(index, [&] { sim.prepareFixedWorkspace(); });
        } else {
            instrumentation::Observation observed;
            observed.poison = true;
            {
                const instrumentation::Scope scope(observed);
                sim.prepareFixedWorkspace();
            }
            require(sim.fixedWorkspaceStartsClear(), "First lambda or inactive Jacobian lanes contain allocator poison");
            fail_points = observed.allocations;
            std::cout << "mode=" << (driven ? "DRIVEN" : "FREE") << " bodies=" << shape.bodies
                      << " objects=" << shape.constraints << " rows=" << shape.rows << " generators=" << shape.generators
                      << " allocation_points=" << fail_points << '\n';
        }
    };
    candidate(0);
    require(fail_points >= 45, "Solver workspace allocation measurement did not cover the reserve sequence");
    for (std::size_t index = 1; index <= fail_points; ++index) {
        candidate(index);
        compare(render(active.get(), 32, nullptr), render(twin.get(), 32, nullptr));
    }
    candidate(0);
    std::cout << "Every allocation failure preserved active PCM and telemetry through the next block.\n";
}
}

int main(int argc, char **argv) try {
    require(argc == 3, "Expected workspace suite and fixture");
    const auto fixture = fs::absolute(argv[2]);
    if (std::string_view(argv[1]) == "cold") {
        cold(fixture, fixture.parent_path());
        if (const auto *reference = std::getenv("ES_FLEET_REFERENCE")) {
            const auto source = fs::path(reference) / "source";
            cold(source / "assets/engines/atg-video-2/07_gm_ls.json", source);
        }
    } else {
        rollback_arrays();
        rollback_graph(fixture, fixture.parent_path(), false);
        rollback_graph(fixture, fixture.parent_path(), true);
    }
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
