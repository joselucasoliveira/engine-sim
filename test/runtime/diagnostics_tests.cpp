#include "legacy_runtime.hpp"
#include "runtime.hpp"
#include "simulation_fault.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <new>
#include <vector>

namespace {
thread_local long allocationBudget = -1;
thread_local unsigned failedAllocations = 0;
struct AllocationFailure {
    long previous;
    explicit AllocationFailure(long budget) : previous(allocationBudget) { allocationBudget = budget; }
    ~AllocationFailure() { allocationBudget = previous; }
};
}
#if defined(_MSC_VER)
#define ES_DIAGNOSTIC_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define ES_DIAGNOSTIC_NOINLINE __attribute__((noinline))
#else
#define ES_DIAGNOSTIC_NOINLINE
#endif
ES_DIAGNOSTIC_NOINLINE void *operator new(std::size_t bytes) {
    if (allocationBudget == -2 || allocationBudget == 0) {
        if (allocationBudget == 0) allocationBudget = -1;
        ++failedAllocations;
        throw std::bad_alloc();
    }
    if (allocationBudget > 0) --allocationBudget;
    if (auto *pointer = std::malloc(bytes ? bytes : 1)) return pointer;
    throw std::bad_alloc();
}
ES_DIAGNOSTIC_NOINLINE void operator delete(void *pointer) noexcept { std::free(pointer); }
ES_DIAGNOSTIC_NOINLINE void operator delete(void *pointer, std::size_t) noexcept { std::free(pointer); }

namespace engine_sim {
struct RuntimeTestAccess {
    static PullEngine &pull(es_runtime &rt) { return *rt.implementation.m_session->pull; }
};
}
namespace {
namespace fs = std::filesystem;
using Handle = std::unique_ptr<es_runtime, decltype(&es_destroy)>;
void checked(es_status status, const es_error &error) {
    if (status != ES_OK) throw std::runtime_error(error.message);
}
es_telemetry snapshot(es_runtime *handle) {
    es_telemetry value{}; value.size = sizeof(value); value.abi = ES_ABI_V2;
    es_error error{}; checked(es_get_telemetry(handle, &value, &error), error); return value;
}
void controls(es_runtime *handle) {
    const es_controls value{sizeof(value), ES_ABI_V2, 1, 1, .3, 0, 0};
    es_error error{}; checked(es_set_controls(handle, &value, &error), error);
}
std::vector<float> render(es_runtime *handle, unsigned frames, unsigned partition = 257) {
    std::vector<float> output(frames);
    for (unsigned offset = 0; offset < frames;) {
        const auto count = std::min(partition, frames - offset); std::uint32_t written = 99;
        es_error error{}; checked(es_render(handle, count, output.data() + offset, count, &written, &error), error);
        if (written != count) throw std::runtime_error("Successful render lost a requested frame");
        offset += written;
    }
    return output;
}
void same(const std::vector<float> &actual, const std::vector<float> &expected) {
    ASSERT_EQ(actual.size(), expected.size());
    EXPECT_EQ(std::memcmp(actual.data(), expected.data(), actual.size() * sizeof(float)), 0);
}
std::string read(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(fs::file_size(path)), '\0');
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("Cannot read diagnostic fixture");
    return bytes;
}
void write(const fs::path &path, const std::string &bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close(); if (!output) throw std::runtime_error("Cannot write diagnostic fixture");
}
void replace(std::string &text, const std::string &from, const std::string &to) {
    const auto offset = text.find(from); if (offset == std::string::npos) throw std::runtime_error("Missing diagnostic fixture field");
    text.replace(offset, from.size(), to);
}
class RuntimeDiagnostics : public testing::Test {
protected:
    std::array<fs::path, 2> roots;
    void SetUp() override {
        const auto *source = std::getenv("ENGINE_SIM_TEST_SOURCE"); ASSERT_NE(source, nullptr);
        const auto base = fs::current_path() / fs::u8path(std::string(testing::UnitTest::GetInstance()->current_test_info()->name()) + u8" espaço Ω");
        for (unsigned i = 0; i < roots.size(); ++i) {
            roots[i] = base / std::to_string(i); fs::create_directories(roots[i]);
            fs::copy(fs::u8path(source) / "examples/runtime/fixture", roots[i], fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            auto script = read(roots[i] / "main.mr");
            replace(script, "noise: 0.0, jitter: 0.0", "noise: 0.25, jitter: 0.2");
            replace(script, "burning_efficiency_randomness: 0.0", "burning_efficiency_randomness: 0.15");
            if (i) { replace(script, "throw: 0.040", "throw: 0.035"); replace(script, "simulation_frequency: 10000", "simulation_frequency: 20000"); }
            write(roots[i] / "main.mr", script); write(roots[i] / "broken.mr", "broken (\n");
        }
    }
    es_status load(es_runtime *handle, unsigned index, es_error &error, const char *entry = "main.mr") {
        return engine_sim::legacy_runtime::load(handle, (roots[index] / entry).u8string().c_str(), (roots[index] / "library").u8string().c_str(), roots[index].u8string().c_str(), &error);
    }
    Handle prepare(unsigned index = 0, std::uint64_t seed = 17, unsigned rate = 44100) {
        es_config config{}; es_error error{}; checked(es_config_init(ES_ABI_V2, sizeof(config), &config, &error), error);
        config.seed = seed; config.output_hz = rate; es_runtime *raw = nullptr;
        checked(es_create(&config, &raw, &error), error); Handle handle(raw, es_destroy);
        checked(load(raw, index, error), error); return handle;
    }
};

TEST_F(RuntimeDiagnostics, HundredRetirementCyclesKeepDefaultResetAndFailedReplacement) {
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        SCOPED_TRACE(cycle);
        const auto index = cycle % 2, rate = index ? 44100U : 48000U;
        auto active = prepare(index, cycle, rate), reference = prepare(index, cycle, rate);
        controls(active.get()); controls(reference.get()); same(render(active.get(), 257), render(reference.get(), 257, 1));
        const auto before = snapshot(active.get()); es_error error{};
        EXPECT_EQ(load(active.get(), index, error, "broken.mr"), engine_sim::legacy_runtime::script_error) << error.message;
        const auto after = snapshot(active.get()); EXPECT_EQ(std::memcmp(&before, &after, sizeof(before)), 0);
        same(render(active.get(), 64), render(reference.get(), 64, 7));
        checked(engine_sim::legacy_runtime::reset(active.get(), &error), error); checked(engine_sim::legacy_runtime::reset(reference.get(), &error), error);
        EXPECT_EQ(snapshot(active.get()).frames, 0U); same(render(active.get(), 64), render(reference.get(), 64));
        if (cycle % 3 == 0) {
            engine_sim::RuntimeTestAccess::pull(*active).setTickObserver([](const Simulator &) { throw engine_sim::SimulationFault("Diagnostic retirement fault"); });
            std::array<float, 64> output; output.fill(42); std::uint32_t written = 99;
            EXPECT_EQ(es_render(active.get(), output.size(), output.data(), output.size(), &written, &error), ES_RENDER_FAULT);
            EXPECT_LT(written, output.size()); EXPECT_EQ(snapshot(active.get()).state, ES_FAULTED);
            EXPECT_TRUE(std::all_of(output.begin() + written, output.end(), [](float value) { return value == 42; }));
            checked(engine_sim::legacy_runtime::reset(active.get(), &error), error); EXPECT_EQ(snapshot(active.get()).frames, 0U);
        }
        checked(es_unload(active.get(), &error), error); checked(es_unload(active.get(), &error), error);
        EXPECT_EQ(snapshot(active.get()).state, ES_EMPTY); EXPECT_EQ(engine_sim::legacy_runtime::reset(active.get(), &error), ES_STATE);
        checked(load(active.get(), index, error), error); EXPECT_EQ(snapshot(active.get()).frames, 0U);
    }
}
TEST_F(RuntimeDiagnostics, LibraryPreparationAllocationFailurePreservesActiveContinuation) {
    auto active = prepare(), reference = prepare(); controls(active.get()); controls(reference.get());
    same(render(active.get(), 257), render(reference.get(), 257));
    const auto entry = (roots[1] / "main.mr").u8string(), library = (roots[1] / "library").u8string(), assets = roots[1].u8string();
    for (long budget : {0L, 1L, 2L, 7L, 31L}) {
        const auto before = snapshot(active.get()); const auto failures = failedAllocations; es_error error{}; es_status status;
        { AllocationFailure fail(budget); status = engine_sim::legacy_runtime::load(active.get(), entry.c_str(), library.c_str(), assets.c_str(), &error); }
        ASSERT_GT(failedAllocations, failures); EXPECT_EQ(status, ES_NO_MEMORY) << error.message;
        EXPECT_NE(error.message[0], '\0'); EXPECT_NE(std::memchr(error.message, 0, sizeof(error.message)), nullptr);
        const auto after = snapshot(active.get()); EXPECT_EQ(std::memcmp(&before, &after, sizeof(before)), 0);
        same(render(active.get(), 128), render(reference.get(), 128, 1));
    }
    es_config config{}; es_error error{}; checked(es_config_init(ES_ABI_V2, sizeof(config), &config, &error), error);
    es_runtime *rejected = reinterpret_cast<es_runtime *>(std::uintptr_t{1}); es_status status;
    { AllocationFailure persistent(-2); status = es_create(&config, &rejected, &error); }
    EXPECT_EQ(status, ES_NO_MEMORY); EXPECT_EQ(rejected, nullptr); EXPECT_NE(error.message[0], '\0');
}
TEST_F(RuntimeDiagnostics, AllocationFaultCommitsOnlyCompleteIntervalsAtBothOutputRates) {
    for (unsigned rate : {44100U, 48000U}) {
        auto active = prepare(0, 81, rate); controls(active.get()); unsigned ticks = 0;
        engine_sim::RuntimeTestAccess::pull(*active).setTickObserver([&](const Simulator &) {
            if (++ticks == 5) { AllocationFailure fail(0); void *memory = ::operator new(64); ::operator delete(memory); }
        });
        std::array<float, 66> output; output.fill(42); std::uint32_t written = 99; es_error error{};
        EXPECT_EQ(es_render(active.get(), 64, output.data() + 1, 64, &written, &error), ES_NO_MEMORY);
        const auto expected = (5U * rate + 9999U) / 10000U - 1U;
        EXPECT_EQ(written, expected); EXPECT_EQ(output.front(), 42); EXPECT_EQ(output.back(), 42);
        EXPECT_TRUE(std::all_of(output.begin() + 1 + written, output.end(), [](float value) { return value == 42; }));
        const auto state = snapshot(active.get()); EXPECT_EQ(state.state, ES_FAULTED); EXPECT_EQ(state.frames, written);
        EXPECT_EQ(state.ticks, std::uint64_t(written) * 10000 / rate);
        EXPECT_EQ(state.native_frames, (std::uint64_t(written) * 44100 + rate - 1) / rate);
        written = 99; EXPECT_EQ(es_render(active.get(), 0, nullptr, 0, &written, &error), ES_STATE); EXPECT_EQ(written, 0U);
        checked(engine_sim::legacy_runtime::reset(active.get(), &error), error); auto fresh = prepare(0, 81, rate);
        same(render(active.get(), 441), render(fresh.get(), 441, 7));
    }
}
TEST_F(RuntimeDiagnostics, ConcurrentRenderOwnersSurviveOtherRootPreparationAndRetirement) {
    auto a = prepare(0, 17), b = prepare(1, 37, 48000), referenceA = prepare(0, 17), referenceB = prepare(1, 37, 48000);
    for (auto *handle : {a.get(), b.get(), referenceA.get(), referenceB.get()}) controls(handle);
    const auto expectedA = render(referenceA.get(), 44100, 503), expectedB = render(referenceB.get(), 48000, 7);
    std::mutex mutex; std::condition_variable change; unsigned ready = 0; bool start = false;
    std::atomic<bool> preparing{false}; std::atomic<unsigned> overlappingTicks{0};
    auto bind = [&](es_runtime &handle) {
        engine_sim::RuntimeTestAccess::pull(handle).setTickObserver([&, first = true](const Simulator &) mutable {
            if (first) {
                first = false; std::unique_lock<std::mutex> lock(mutex); ++ready; change.notify_all();
                if (!change.wait_for(lock, std::chrono::seconds(10), [&] { return start; })) throw std::runtime_error("Diagnostic owner rendezvous timed out");
            }
            if (preparing.load()) ++overlappingTicks;
        });
    };
    bind(*a); bind(*b);
    auto taskA = std::async(std::launch::async, [&] { return render(a.get(), 44100, 64); });
    auto taskB = std::async(std::launch::async, [&] { return render(b.get(), 48000, 257); });
    bool entered;
    { std::unique_lock<std::mutex> lock(mutex); entered = change.wait_for(lock, std::chrono::seconds(10), [&] { return ready == 2; }); preparing = true; start = true; }
    change.notify_all(); ASSERT_TRUE(entered);
    for (unsigned cycle = 0; cycle < 20; ++cycle) { auto unrelated = prepare(cycle % 2, 900 + cycle); controls(unrelated.get()); render(unrelated.get(), 257); }
    preparing = false; same(taskA.get(), expectedA); same(taskB.get(), expectedB); EXPECT_GT(overlappingTicks.load(), 1U);
    // future::get is the owner barrier; the main thread may now resume each one.
    same(render(a.get(), 7), render(referenceA.get(), 7)); same(render(b.get(), 7), render(referenceB.get(), 7));
}
}
