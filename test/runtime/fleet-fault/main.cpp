#include "json_runtime.hpp"
#include "runtime.hpp"

#ifdef ES_R3_FAULT_OBSERVER
#include "observe.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>

namespace engine_sim {
struct RuntimeTestAccess {
    static PullEngine &pull(es_runtime &runtime) { return *runtime.implementation.m_session->pull; }
    static Simulator &simulator(es_runtime &runtime) { return *runtime.implementation.m_session->prepared.session->simulator; }
    static void poisonQuantizer(es_runtime &runtime) {
        runtime.implementation.m_session->prepared.session->synthesis.m_renderParameters.volume =
            std::numeric_limits<float>::quiet_NaN();
    }
};

struct ClockTestAccess {
    static void before(PullEngine &pull, PullEngine::ControlLatch action) {
        pull.m_beforeTick = [previous = std::move(pull.m_beforeTick), action = std::move(action)] {
            if (previous) previous();
            action();
        };
    }
    static void after(PullEngine &pull, PullEngine::ControlLatch action) {
        pull.m_checkedAfterTick = [previous = std::move(pull.m_checkedAfterTick), action = std::move(action)] {
            const auto result = previous ? previous() : ProcessingStatus::ok;
            if (result == ProcessingStatus::ok) action();
            return result;
        };
    }
    static void nearWrap(PullEngine &pull) {
        constexpr auto bound = std::numeric_limits<std::uint64_t>::max() - 2;
        pull.m_clock.m_counts = {bound, bound, bound};
    }
};
}

namespace {
namespace fs = std::filesystem;
using engine_sim::ClockTestAccess;
using engine_sim::RuntimeTestAccess;
using Status = engine_sim::ProcessingStatus;
using Owner = std::unique_ptr<es_runtime, decltype(&es_destroy)>;
constexpr auto sentinel = 42.f;
constexpr std::size_t block_size = 64;

enum class Leaf { mechanical, force, solver, quantization, snapshot };

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
void check(es_status status, const es_error &problem) {
    if (status != ES_OK) throw std::runtime_error(problem.message);
}

Owner prepare(const fs::path &model, const fs::path &assets, unsigned rate, bool driven, bool one_tick_per_frame) {
    es_config options{};
    es_error problem{};
    check(es_config_init(ES_ABI_V2, sizeof(options), &options, &problem), problem);
    options.mode = driven ? ES_DRIVEN : ES_FREE;
    options.output_hz = rate;
    options.seed = 403;
    if (one_tick_per_frame) options.simulation_hz = rate;
    es_runtime *runtime = nullptr;
    check(es_create(&options, &runtime, &problem), problem);
    Owner result(runtime, es_destroy);
    check(engine_sim::json_runtime::load_file(runtime, model, assets, nullptr, &problem), problem);
    return result;
}

void controls(es_runtime *voice, bool driven, double rpm = 0, bool saturation = false) {
    const es_controls value{sizeof(es_controls), ES_ABI_V2, 1, driven ? 0u : 1u,
                            saturation ? .2 : .25, saturation ? 100. : .375, rpm};
    es_error problem{};
    check(es_set_controls(voice, &value, &problem), problem);
}

es_telemetry telemetry(es_runtime *voice) {
    es_telemetry result{};
    result.size = sizeof(result);
    result.abi = ES_ABI_V2;
    es_error problem{};
    check(es_get_telemetry(voice, &result, &problem), problem);
    return result;
}

void same_state(const es_telemetry &a, const es_telemetry &b) {
    require(std::memcmp(&a, &b, sizeof(a)) == 0, "Last completed telemetry differs from the healthy twin");
}

template<std::size_t Capacity>
struct Render {
    std::array<float, Capacity + 2> pcm;
    std::uint32_t written = 0;
    es_status status = ES_OK;
    es_error problem{};
};

template<std::size_t Capacity = block_size>
Render<Capacity> render(es_runtime *voice, std::uint32_t count, bool observe = false) {
    Render<Capacity> result;
    result.pcm.fill(sentinel);
#ifdef ES_R3_FAULT_OBSERVER
    realtime_probe::Record activity{};
    if (observe) {
        const realtime_probe::Scope enabled(activity);
        result.status = es_render(voice, count, result.pcm.data() + 1, Capacity, &result.written, &result.problem);
    } else
#else
    (void)observe;
#endif
    {
        result.status = es_render(voice, count, result.pcm.data() + 1, Capacity, &result.written, &result.problem);
    }
#ifdef ES_R3_FAULT_OBSERVER
    if (observe) {
        const auto hits = std::count_if(activity.begin(), activity.end(), [](const auto &hit) { return hit.count != 0; });
        if (hits) realtime_probe::dump(activity);
        require(hits == 0, "Recoverable processing entered the allocation, release, lock, I/O or unwind observer");
    }
#endif
    require(result.written <= count && result.pcm.front() == sentinel &&
        std::all_of(result.pcm.begin() + 1 + result.written, result.pcm.end(), [](float v) { return v == sentinel; }),
        "Render wrote an incomplete sample or changed the destination outside its prefix");
    return result;
}

struct RejectingSolver : atg_scs::SleSolver {
    RejectingSolver() : SleSolver(true) {}
    bool solveWithLimits(atg_scs::SparseMatrix<3> &, atg_scs::Matrix &, atg_scs::Matrix &,
                         atg_scs::Matrix &, atg_scs::Matrix *, atg_scs::Matrix *) override {
        ++calls;
        return false;
    }
    unsigned calls = 0;
};

Status installFault(es_runtime &runtime, RejectingSolver &solver, Leaf leaf, unsigned prefix) {
    auto &pull = RuntimeTestAccess::pull(runtime);
    auto &sim = RuntimeTestAccess::simulator(runtime);
    auto &crank = sim.getEngine()->getOutputCrankshaft()->m_body;
    if (leaf == Leaf::mechanical) {
        ClockTestAccess::before(pull, [&crank, left = prefix]() mutable {
            if (left-- == 0) crank.theta = std::numeric_limits<double>::infinity();
        });
        return Status::mechanical_state;
    }
    if (leaf == Leaf::force) {
        auto &gas = sim.getEngine()->getChamber(0)->m_system;
        ClockTestAccess::before(pull, [&gas, left = prefix]() mutable {
            if (left-- == 0) gas.changePressure(std::numeric_limits<double>::quiet_NaN());
        });
        return Status::chamber_force;
    }
    if (leaf == Leaf::solver) {
        auto &system = static_cast<atg_scs::OptimizedNsvRigidBodySystem &>(*sim.getSystem());
        ClockTestAccess::before(pull, [&system, &solver, left = prefix]() mutable {
            if (left-- == 0) system.initialize(&solver);
        });
        return Status::solver_convergence;
    }
    if (leaf == Leaf::quantization) {
        if (prefix == 0) RuntimeTestAccess::poisonQuantizer(runtime);
        else ClockTestAccess::after(pull, [&runtime, left = prefix]() mutable {
            if (--left == 0) RuntimeTestAccess::poisonQuantizer(runtime);
        });
        return Status::quantization;
    }
    // The injected tick has already completed all numerical and drive checks.
    ClockTestAccess::after(pull, [&crank, left = prefix]() mutable {
        if (left-- == 0) crank.v_theta = std::numeric_limits<double>::quiet_NaN();
    });
    return Status::telemetry_nonfinite;
}

void checkPrefix(const fs::path &model, unsigned rate, bool driven, unsigned prefix, Leaf leaf, bool observe) {
    RejectingSolver solver;
    const auto root = model.parent_path();
    auto active = prepare(model, root, rate, driven, true);
    auto healthy = prepare(model, root, rate, driven, true);
    controls(active.get(), driven);
    controls(healthy.get(), driven);
    const auto reason = installFault(*active, solver, leaf, prefix);
    const auto reference = render(healthy.get(), prefix);
    check(reference.status, reference.problem);
    const auto before = telemetry(healthy.get());
    const auto failed = render(active.get(), block_size, observe);
    require(failed.status == ES_RENDER_FAULT && failed.written == prefix,
            "Processing failure did not report its exact completed prefix");
    require(std::string_view(failed.problem.message) == engine_sim::processingMessage(reason) &&
            active->implementation.processingFailure() == reason,
            "Failure location or fixed v2 error text differs from the chosen leaf");
    require(std::memcmp(failed.pcm.data(), reference.pcm.data(), (prefix + 1) * sizeof(float)) == 0,
            "Good PCM prefix differs from the isolated twin");
    auto last = telemetry(active.get());
    require(last.state == ES_FAULTED && last.frames == prefix, "Failure did not latch an exact FAULTED snapshot");
    last.state = ES_READY;
    same_state(last, before);
    if (leaf == Leaf::solver) require(solver.calls == 1, "Failed solver was retried or skipped");
    const auto retry = render(active.get(), 1, observe);
    require(retry.status == ES_STATE && retry.written == 0, "Faulted voice resumed partially executed state");
    const auto continues = render(healthy.get(), block_size);
    check(continues.status, continues.problem);
    auto independent = prepare(model, root, rate, driven, true);
    controls(independent.get(), driven);
    const auto independent_prefix = render(independent.get(), prefix);
    check(independent_prefix.status, independent_prefix.problem);
    const auto next = render(independent.get(), block_size);
    check(next.status, next.problem);
    require(std::memcmp(continues.pcm.data(), next.pcm.data(), sizeof(next.pcm)) == 0, "Failure perturbed another voice");
    same_state(telemetry(healthy.get()), telemetry(independent.get()));
    es_error reset_error{};
    check(engine_sim::json_runtime::reset(active.get(), nullptr, &reset_error), reset_error);
    require(active->implementation.processingFailure() == Status::ok && telemetry(active.get()).frames == 0,
            "Fresh detached reset retained a stale reason or advanced frames");
    controls(active.get(), driven);
    const auto recovered = render(active.get(), prefix, observe);
    check(recovered.status, recovered.problem);
    require(std::memcmp(recovered.pcm.data(), reference.pcm.data(), sizeof(recovered.pcm)) == 0,
            "Fresh reset did not reproduce original PCM");
    std::cout << "rate=" << rate << " mode=" << (driven ? "DRIVEN" : "FREE") << " completed=" << prefix
              << " ticks=" << before.ticks << " native=" << before.native_frames
              << " crank=" << std::hexfloat << before.crank_rad_s << " cycle=" << before.cycle_rad
              << " requested_load=" << before.requested_load_nm << " load_reaction=" << before.load_reaction_nm
              << std::defaultfloat << " reason=" << engine_sim::processingMessage(reason) << '\n';
}

void preflight(const fs::path &model) {
    auto active = prepare(model, model.parent_path(), 44100, false, true);
    const auto original = telemetry(active.get());
    auto &pull = RuntimeTestAccess::pull(*active);
    ClockTestAccess::nearWrap(pull);
    const auto before = pull.counts();
    const auto capacity = render(active.get(), 3, true);
    require(capacity.status == ES_CAPACITY && capacity.written == 0, "Counter overflow failed as an in-flight error");
    require(pull.counts().frames == before.frames && pull.counts().ticks == before.ticks &&
        pull.counts().nativeFrames == before.nativeFrames, "Counter preflight mutated source progress");
    same_state(telemetry(active.get()), original);
    std::uint32_t written = 99;
    es_error problem{};
    require(es_render(active.get(), 1, nullptr, 1, &written, &problem) == ES_INVALID && written == 0,
            "Malformed span entered the source");
    same_state(telemetry(active.get()), original);
}

void natural(const fs::path &source, bool observe) {
    const auto model = source / "assets/engines/atg-video-1/02_kohler_ch750.json";
    auto active = prepare(model, source, 44100, true, false);
    auto twin = prepare(model, source, 44100, true, false);
    controls(active.get(), true, 0, true);
    controls(twin.get(), true, 0, true);
    const auto a = render<2205>(active.get(), 2205);
    const auto b = render<2205>(twin.get(), 2205);
    check(a.status, a.problem);
    check(b.status, b.problem);
    require(std::memcmp(a.pcm.data(), b.pcm.data(), sizeof(a.pcm)) == 0, "Natural witness setup changed");
    const auto good = render(twin.get(), 1);
    check(good.status, good.problem);
    controls(active.get(), true, 3600, true);
    const auto fault = render(active.get(), block_size, observe);
    require(fault.status == ES_RENDER_FAULT && fault.written == 1 &&
        std::memcmp(&fault.pcm[1], &good.pcm[1], sizeof(float)) == 0 &&
        active->implementation.processingFailure() == Status::drive_saturation,
        "Frozen Kohler driven saturation first frame or reason differs");
    auto last = telemetry(active.get());
    last.state = ES_READY;
    same_state(last, telemetry(twin.get()));
    std::cout << "frozen Kohler witness: 2205 setup frames, 1 completed frame, ticks=" << last.ticks
              << " native=" << last.native_frames << " crank=" << std::hexfloat << last.crank_rad_s
              << " cycle=" << last.cycle_rad << std::defaultfloat << " reason=" << fault.problem.message << '\n';
}
}

int main(int argc, char **argv) try {
    require(argc >= 3, "Expected suite and input fixture");
    const auto model = fs::absolute(argv[2]);
    const bool observe = std::string_view(argv[1]) == "no-unwind";
    preflight(model);
    for (auto rate : {44100u, 48000u}) {
        for (bool driven : {false, true}) {
            for (auto prefix : {0u, 17u, 63u}) {
                for (auto leaf : {Leaf::mechanical, Leaf::force, Leaf::solver, Leaf::quantization, Leaf::snapshot})
                    checkPrefix(model, rate, driven, prefix, leaf, observe);
            }
        }
    }
    if (observe) {
        require(argc == 4, "Missing source for natural fault witness");
        const auto *reference = std::getenv("ES_FLEET_REFERENCE");
        natural(reference ? fs::path(reference) / "source" : fs::absolute(argv[3]), observe);
#ifdef ES_R3_FAULT_OBSERVER
        std::cout << "GNU ELF wrapped call observer armed for all attempts. Zero observed entries.\n";
#else
        std::cout << "Functional diagnostics only. GNU ELF call observer unavailable on this build.\n";
#endif
    }
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
