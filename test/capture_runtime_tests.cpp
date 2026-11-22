#include "../src/runtime/capture_runtime.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace {
using engine_sim_capture::RuntimeHost;
namespace fs = std::filesystem;
std::unique_ptr<RuntimeHost> fixture(std::uint64_t seed = 1) {
    const auto *root = std::getenv("ENGINE_SIM_TEST_SOURCE");
    if (!root) throw std::runtime_error("Missing test fixture root");
    const auto base = fs::path(root) / "examples/runtime/fixture";
    return RuntimeHost::prepare({base / "main.mr", base / "library", base}, seed, true);
}
TEST(CaptureRuntime, ExactFramesTicksAndPartitionedDiagnostics) {
    auto whole = fixture(), parts = fixture();
    std::array<float, 441> a{}, b{};
    std::uint64_t expectedTicks = 0;
    for (unsigned block = 0; block < 30; ++block) {
        const engine_sim_capture::Controls controls{block < 15 ? 0.1 : 0.3, 0, block < 20, true, false};
        whole->setControls(controls); parts->setControls(controls);
        whole->beginBlock(); parts->beginBlock();
        std::uint32_t written = 0;
        whole->render(a.data(), a.size(), written);
        ASSERT_EQ(written, a.size());
        unsigned offset = 0;
        for (unsigned count : {1U, 7U, 251U, 31U, 151U}) {
            written = 0; parts->render(b.data() + offset, count, written);
            ASSERT_EQ(written, count); offset += count;
        }
        whole->endBlock(); parts->endBlock();
        EXPECT_EQ(a, b);
        const auto &x = whole->diagnostics(), &y = parts->diagnostics();
        ASSERT_EQ(x.ticks, y.ticks); ASSERT_GT(x.ticks, 0U);
        for (std::size_t i = 0; i < x.ticks; ++i) {
            EXPECT_EQ(x.samples[i].tick, ++expectedTicks);
            EXPECT_EQ(x.samples[i].tick, y.samples[i].tick);
            EXPECT_EQ(x.samples[i].lit, y.samples[i].lit);
            EXPECT_EQ(x.samples[i].cycle, y.samples[i].cycle);
            EXPECT_EQ(x.samples[i].speed, y.samples[i].speed);
            EXPECT_EQ(x.samples[i].demand, controls.demand);
            EXPECT_EQ(x.samples[i].demand, y.samples[i].demand);
        }
        EXPECT_EQ(x.pressure.min, y.pressure.min); EXPECT_EQ(x.pressure.max, y.pressure.max);
        EXPECT_EQ(whole->telemetry().frames, (block + 1) * 441);
        EXPECT_EQ(whole->telemetry().ticks, std::uint64_t(block + 1) * whole->telemetry().simulation_hz / 100);
        EXPECT_EQ(whole->telemetry().native_frames, whole->telemetry().frames);
    }
}
TEST(CaptureRuntime, ZeroCapacityAndFailedPreparationKeepTimeline) {
    auto active = fixture();
    std::array<float, 443> guarded; guarded.fill(7);
    std::uint32_t written = 0;
    EXPECT_THROW(active->render(guarded.data() + 1, 1, written), std::invalid_argument);
    active->beginBlock();
    active->render(nullptr, 0, written);
    EXPECT_EQ(active->telemetry().frames, 0U);
    EXPECT_THROW(active->render(guarded.data() + 1, 442, written), std::invalid_argument);
    EXPECT_EQ(active->telemetry().frames, 0U);
    active->render(guarded.data() + 1, 441, written); active->endBlock();
    EXPECT_EQ(written, 441U); EXPECT_EQ(guarded.front(), 7); EXPECT_EQ(guarded.back(), 7);
    const auto before = active->telemetry();
    const auto base = fs::path(std::getenv("ENGINE_SIM_TEST_SOURCE")) / "examples/runtime/fixture";
    EXPECT_THROW(RuntimeHost::prepare({base / "missing.mr", base / "library", base}, 1), EngineLoadError);
    EXPECT_EQ(active->telemetry().frames, before.frames);
    active->beginBlock(); written = 0;
    active->render(guarded.data() + 1, 441, written); active->endBlock();
    EXPECT_EQ(active->telemetry().frames, 882U);
}
TEST(CaptureRuntime, IndependentSeedsAndInvalidControlsPreservePendingVector) {
    auto a = fixture(0), b = fixture(0), other = fixture(UINT64_MAX);
    a->setControls({0.2, 0, true, true, false}); b->setControls({0.2, 0, true, true, false});
    EXPECT_THROW(a->setControls({std::nan(""), 0, false, false, false}), std::invalid_argument);
    std::array<float, 441> x{}, y{}, noise{};
    for (unsigned block = 0; block < 20; ++block) {
        for (auto *owner : {a.get(), other.get(), b.get()}) {
            owner->beginBlock(); std::uint32_t written = 0;
            owner->render(owner == a.get() ? x.data() : owner == b.get() ? y.data() : noise.data(), 441, written);
            owner->endBlock(); ASSERT_EQ(written, 441U);
        }
        EXPECT_EQ(x, y);
    }
    EXPECT_EQ(a->telemetry().ticks, b->telemetry().ticks);
    EXPECT_EQ(a->telemetry().crank_rad_s, b->telemetry().crank_rad_s);
}
}
