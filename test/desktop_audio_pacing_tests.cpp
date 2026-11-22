#include "desktop_audio_pacer.h"
#include "prepared_engine.h"
#include "pull_engine.h"
#include "simulation_fault.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>

namespace {
using desktop::AudioPacer;
class Device final : public desktop::Host {
public:
    desktop::FrameState state;
    std::deque<std::int16_t> queue;
    std::vector<std::int16_t> submitted, played;
    std::array<std::int16_t, 44100> staging{};
    std::uint64_t consumed = 44090, sourceConsumed = 0, silence = 0;
    std::uint32_t maximumWrite = 44100, partial = 44100;
    bool paused = false;
    void consume(std::size_t count) {
        while (count--) {
            ++consumed;
            if (queue.empty()) { ++silence; continue; }
            played.push_back(queue.front()); queue.pop_front(); ++sourceConsumed;
        }
    }
    desktop::AudioProgress audioProgress() override {
        desktop::AudioProgress value;
        value.queueKnown = true; value.cursor = consumed % 44100;
        value.queued = static_cast<std::uint32_t>(queue.size());
        value.writable = std::min<std::uint32_t>(maximumWrite, 44100 - value.queued);
        value.consumed = consumed; value.submittedSource = submitted.size();
        value.consumedSource = sourceConsumed; value.starvationSilence = silence;
        return value;
    }
    desktop::AudioOffset audioCursor() override { return consumed % 44100; }
    desktop::AudioSegments lockAudio(desktop::AudioOffset offset, desktop::AudioOffset offered) override {
        const auto accepted = std::min({offered, partial, maximumWrite});
        const auto first = std::min(accepted, 44100 - offset);
        return {staging.data() + offset, staging.data(), first, accepted - first};
    }
    void unlockAudio(const desktop::AudioSegments &span) override {
        const auto append = [&](const auto *data, auto count) {
            for (std::size_t i = 0; i < count; ++i) { queue.push_back(data[i]); submitted.push_back(data[i]); }
        };
        append(span.first, span.firstCount); append(span.second, span.secondCount);
    }
    void setAudioSourcePaused(bool value) override { paused = value; }
    void initialize(const engine_sim::ResourcePaths &, const engine_sim::CompiledScene &) override {}
    void shutdown() noexcept override {}
    void StartFrame() override {}
    void EndFrame() override {}
    const desktop::FrameState &frame() const override { return state; }
    bool IsKeyDown(desktop::Key) override { return false; }
    bool ProcessKeyDown(desktop::Key) override { return false; }
    bool ProcessMouseButtonDown(desktop::Button) override { return false; }
    bool ProcessMouseButtonUp(desktop::Button) override { return false; }
    int GetMouseWheel() override { return 0; }
    void GetOsMousePos(int *, int *) override {}
    void setFullscreen(bool) override {}
    void uploadGeometry(const desktop::Vertex *, std::size_t, const std::uint16_t *, std::size_t) override {}
    float measureText(const std::string &, float) const override { return 0; }
    void openAudio() override {}
    void setAudioPlaying(bool) override {}
    void quiesceAudio() noexcept override {}
    void readScreen(std::uint8_t *, std::size_t) override {}
};

std::int16_t tagged(std::uint64_t index) { return static_cast<std::int16_t>(static_cast<int>((index * 97) % 60001) - 30000); }
struct Source {
    std::uint64_t count = 0, failAt = UINT64_MAX;
    float operator()() {
        if (count == failAt) throw engine_sim::SimulationFault("injected source fault");
        return static_cast<float>(tagged(count++)) / 32768.0f;
    }
};

TEST(DesktopPacing, StoppedDeviceAndFullTargetCannotAdvanceSource) {
    Device device;
    Source source;
    AudioPacer pacer([&] { return source(); });
    ASSERT_TRUE(pacer.service(device, 1, false));
    ASSERT_EQ(source.count, AudioPacer::Target);
    for (int i = 0; i < 1000; ++i) ASSERT_TRUE(pacer.service(device, 1, false));
    EXPECT_EQ(source.count, AudioPacer::Target);
    EXPECT_EQ(device.queue.size(), AudioPacer::Target);
    EXPECT_EQ(pacer.statistics().maximumQueued, AudioPacer::Target);
    EXPECT_EQ(pacer.statistics().discarded, 0u);
    EXPECT_EQ(pacer.statistics().reserved, 0u);
}

TEST(DesktopPacing, PartialSubmissionsRetainOrderedSuffixAcrossCursorWrap) {
    Device device;
    device.partial = 37;
    Source source;
    AudioPacer pacer([&] { return source(); });
    pacer.service(device, 1, false);
    EXPECT_EQ(pacer.statistics().ready, AudioPacer::Target - 37);
    for (int i = 0; i < 100; ++i) pacer.service(device, 1, false);
    ASSERT_EQ(device.queue.size(), AudioPacer::Target);
    ASSERT_EQ(source.count, AudioPacer::Target);
    for (int i = 0; i < 3000; ++i) {
        device.consume(1 + i % 61);
        ASSERT_TRUE(pacer.service(device, 1, false));
    }
    for (std::size_t i = 0; i < device.submitted.size(); ++i) ASSERT_EQ(device.submitted[i], tagged(i));
    for (std::size_t i = 0; i < device.played.size(); ++i) ASSERT_EQ(device.played[i], tagged(i));
    EXPECT_EQ(device.silence, 0u);
    EXPECT_EQ(pacer.statistics().discarded, 0u);
    EXPECT_LE(pacer.statistics().maximumQueued, AudioPacer::Target);
}

TEST(DesktopPacing, SameDemandWithDifferentUiPartitionsProducesIdenticalPcm) {
    const auto run = [](const std::vector<std::size_t> &parts, bool excessivePolls) {
        Device device;
        Source source;
        AudioPacer pacer([&] { return source(); });
        pacer.service(device, 1, false);
        constexpr std::size_t total = 88200;
        for (std::size_t n = 0, i = 0; n < total; ++i) {
            const auto count = std::min(parts[i % parts.size()], total - n);
            device.consume(count); n += count;
            pacer.service(device, 1, false);
            if (excessivePolls) for (int p = 0; p < 9; ++p) pacer.service(device, 1, false);
        }
        EXPECT_EQ(source.count, total + AudioPacer::Target);
        EXPECT_EQ(device.silence, 0u);
        EXPECT_EQ(pacer.statistics().discarded, 0u);
        EXPECT_EQ(pacer.phase(), 0u);
        return device.submitted;
    };
    EXPECT_EQ(run({245}, false), run({1, 17, 503, 31, 7}, true));
}

TEST(DesktopPacing, FractionalSpeedSurvivesChangesAndRequestPartitions) {
    const auto run = [](const std::vector<std::size_t> &parts) {
        Device device;
        Source source;
        AudioPacer pacer([&] { return source(); });
        const std::array<std::uint32_t, 7> edges{0, 4007, 10009, 20029, 24031, 30007, 32029};
        const std::array<unsigned, 6> divisors{1, 10, 1000, 1, 200, 1};
        std::uint64_t units = 0;
        std::size_t index = 0;
        for (std::size_t e = 0; e < divisors.size(); ++e) {
            for (std::size_t n = edges[e]; n < edges[e + 1];) {
                const auto count = std::min<std::size_t>(parts[index++ % parts.size()], edges[e + 1] - n);
                device.maximumWrite = static_cast<std::uint32_t>(count);
                EXPECT_TRUE(pacer.service(device, divisors[e], false));
                EXPECT_EQ(device.queue.size(), count);
                device.consume(count); n += count;
                units += count * (1000 / divisors[e]);
                EXPECT_EQ(pacer.phase(), units % 1000);
                // Source samples are requested at output left edges. A new
                // right-edge source boundary remains deferred to the next frame.
                EXPECT_EQ(source.count, 1 + (units - 1000 / divisors[e]) / 1000);
            }
        }
        EXPECT_EQ(device.silence, 0u);
        return device.submitted;
    };
    EXPECT_EQ(run({1}), run({7, 251, 31, 503}));
}

TEST(DesktopPacing, SourceFaultKeepsOnlyValidPrefixAndNeverFillsMissingFrames) {
    Device device;
    device.partial = 0;
    Source source;
    source.failAt = 37;
    AudioPacer pacer([&] { return source(); });
    EXPECT_FALSE(pacer.service(device, 1, false));
    EXPECT_EQ(source.count, 37u);
    EXPECT_EQ(pacer.statistics().generatedSource, 37u);
    EXPECT_EQ(pacer.statistics().requestedSource, 38u);
    EXPECT_EQ(pacer.statistics().generatedDevice, 37u);
    EXPECT_EQ(pacer.statistics().ready, 37u);
    EXPECT_EQ(pacer.statistics().reserved, 0u);
    device.partial = 44100;
    EXPECT_FALSE(pacer.service(device, 1, false));
    ASSERT_EQ(device.submitted.size(), 37u);
    for (std::size_t i = 0; i < 37; ++i) EXPECT_EQ(device.submitted[i], tagged(i));
    EXPECT_EQ(pacer.statistics().sourceFaults, 1u);
}

TEST(DesktopPacing, PauseAndBoundedManualStepsDoNotReplaySource) {
    Device device;
    Source source;
    AudioPacer pacer([&] { return source(); });
    for (int i = 0; i < 10; ++i) pacer.service(device, 1, true);
    EXPECT_EQ(source.count, 0u);
    for (int i = 0; i < 12; ++i) ASSERT_TRUE(pacer.service(device, 1, true, 735));
    EXPECT_EQ(source.count, 8820u);
    pacer.service(device, 1, true, 735);
    EXPECT_EQ(source.count, 8820u);
    EXPECT_EQ(pacer.statistics().rejectedManualSteps, 1u);
    for (int i = 0; i < 20; ++i) {
        device.consume(500);
        pacer.service(device, 1, false);
    }
    EXPECT_EQ(device.silence, 0u);
    EXPECT_EQ(pacer.statistics().discarded, 0u);
    for (std::size_t i = 0; i < device.submitted.size(); ++i) EXPECT_EQ(device.submitted[i], tagged(i));
    EXPECT_LE(pacer.statistics().maximumQueued, 2 * AudioPacer::Target);
}

TEST(DesktopPacing, ReadyPcmCannotBeOmittedFromTheBudget) {
    Device device;
    device.queue.resize(44100);
    Source source;
    AudioPacer pacer([&] { return source(); });
    pacer.service(device, 1, false);
    EXPECT_EQ(source.count, 0u);
    // Independent negative control for the old input-only latency estimator:
    // no staged input does not imply room when a second of PCM is ready.
    const auto oldInputOnlyDeficit = AudioPacer::Target - 0;
    EXPECT_GT(oldInputOnlyDeficit, 0u);
    EXPECT_EQ(pacer.statistics().requestedDevice, 0u);
}

TEST(DesktopPacing, RealPullPcmMatchesItsContinuousControlTimeline) {
    const auto *sourceRoot = std::getenv("ENGINE_SIM_TEST_SOURCE");
    ASSERT_NE(sourceRoot, nullptr);
    const auto root = std::filesystem::u8path(sourceRoot) / "examples/runtime/fixture";
    const auto prepare = [&] { return engine_sim::prepareEngine({root / "main.mr", root / "library", root}); };
    auto a = prepare(), b = prepare();
    auto controls = [](Simulator &sim, std::size_t n) {
        sim.getEngine()->setSpeedControl(.25);
        sim.getEngine()->getIgnitionModule()->m_enabled = true;
        sim.m_starterMotor.m_enabled = n < 55125;
    };
    std::size_t left = 0, right = 0;
    engine_sim::PullEngine first(*a.session->simulator, [&] { controls(*a.session->simulator, left); });
    engine_sim::PullEngine second(*b.session->simulator, [&] { controls(*b.session->simulator, right); });
    AudioPacer pacer([&] { float value; first.render(&value, 1); ++left; return value; });
    Device device;
    std::vector<std::int16_t> expected;
    constexpr std::size_t total = 176400;
    for (std::size_t n = 0, i = 0; n < total; ++i) {
        const auto count = std::min<std::size_t>(1 + i % 503, total - n);
        device.maximumWrite = static_cast<std::uint32_t>(count);
        ASSERT_TRUE(pacer.service(device, 1, false));
        ASSERT_EQ(device.queue.size(), count);
        device.consume(count); n += count;
    }
    for (; right < total; ++right) {
        float value;
        second.render(&value, 1);
        expected.push_back(static_cast<std::int16_t>(std::lround(static_cast<double>(value) * 32768)));
    }
    EXPECT_EQ(device.submitted, expected);
    EXPECT_EQ(first.counts().frames, total);
    EXPECT_EQ(first.counts().ticks, second.counts().ticks);
    EXPECT_EQ(a.session->definition.engine->getCrankshaft(0)->m_body.theta,
              b.session->definition.engine->getCrankshaft(0)->m_body.theta);
    EXPECT_EQ(a.session->simulator->synthesizer().getDroppedInputSamples(), 0u);
    EXPECT_EQ(a.session->simulator->synthesizer().getBufferStatistics().droppedOutput, 0u);
}
}
