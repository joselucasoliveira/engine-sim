#include "sample_clock.h"
#include "pull_engine.h"
#include "prepared_engine.h"
#include "checked_solver.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <numeric>
#include <vector>

#include "legacy_access.h"

namespace engine_sim {
struct ClockTestAccess {
    static void preload(SampleClock &clock, ClockCounts counts, std::uint32_t phase) {
        clock.m_counts = counts;
        clock.m_phase = phase;
        clock.m_tickActive = phase != 0;
    }
};
}

namespace {
using namespace engine_sim;
constexpr std::uint64_t rate = SampleClock::NativeRate;

struct Source : ClockSource {
    int requested = 0;
    int applied = 0;
    std::uint64_t starts = 0, ticks = 0, samples = 0;
    std::uint64_t failTick = UINT64_MAX, failSample = UINT64_MAX, nonfiniteTick = UINT64_MAX;
    bool record = true;
    std::vector<std::pair<std::uint64_t, int>> controls;
    std::vector<double> inputs;
    std::vector<std::pair<char, std::uint64_t>> events;
    static double endpoint(std::uint64_t tick) { return tick == 3 ? 1000.0 : 0.0; }
    void latch() override {
        applied = requested;
        if (record) { controls.emplace_back(starts, applied); events.emplace_back('L', starts); }
        ++starts;
    }
    bool tick(double *output, std::size_t capacity) override {
        if (output == nullptr || capacity == 0) return false;
        ++ticks;
        if (record) events.emplace_back('T', ticks);
        output[0] = ticks == nonfiniteTick ? std::numeric_limits<double>::quiet_NaN() : endpoint(ticks);
        return ticks != failTick;
    }
    bool native(AcousticExcitation input, std::int16_t &output) override {
        if (record) { inputs.push_back(input.data[0]); events.emplace_back('N', samples); }
        output = static_cast<std::int16_t>(std::lround(input.data[0]));
        return samples++ != failSample;
    }
};

TEST(Clock, IndependentRationalEventsAndControlEdges) {
    for (const auto frequency : {4000u, 10000u, 30000u, 44100u, 96000u}) {
        SCOPED_TRACE(frequency);
        Source source;
        SampleClock clock(frequency, {0.0}, source);
        const std::array<std::size_t, 8> edges{0, 1, 147, 160, 257, 441, 47999, 48007};
        std::vector<float> output(edges.back());
        for (std::size_t e = 0; e + 1 < edges.size(); ++e) {
            source.requested = static_cast<int>(e + 1);
            clock.render(output.data() + edges[e], edges[e + 1] - edges[e]);
            EXPECT_EQ(clock.counts().ticks, edges[e + 1] * frequency / rate);
        }
        ASSERT_EQ(source.inputs.size(), output.size());
        for (std::uint64_t n = 0; n < output.size(); ++n) {
            const auto k = n * frequency / rate;
            const double fraction = static_cast<double>(n * frequency % rate) / rate;
            const double previous = k == 0 ? 0 : Source::endpoint(k - 1);
            const double expected = (1 - fraction) * previous + fraction * Source::endpoint(k);
            EXPECT_DOUBLE_EQ(source.inputs[n], expected);
        }
        for (const auto &latch : source.controls) {
            int expected = 0;
            for (std::size_t e = 0; e + 1 < edges.size(); ++e) {
                if (edges[e] * frequency <= latch.first * rate) expected = static_cast<int>(e + 1);
            }
            EXPECT_EQ(latch.second, expected);
        }
        // Independent timestamp merge: completed ticks precede a coincident
        // native frame; native precedes the next tick's latch at a tie.
        for (std::size_t i = 1; i < source.events.size(); ++i) {
            const auto a = source.events[i - 1], b = source.events[i];
            const auto numerator = [frequency](auto event) {
                return event.second * (event.first == 'N' ? frequency : rate);
            };
            EXPECT_LE(numerator(a), numerator(b));
            if (numerator(a) == numerator(b)) {
                const auto order = [](char kind) { return kind == 'T' ? 0 : kind == 'N' ? 1 : 2; };
                EXPECT_LT(order(a.first), order(b.first));
            }
        }
        EXPECT_EQ(clock.counts().frames, output.size());
        EXPECT_EQ(clock.counts().nativeFrames, output.size());
    }
}

TEST(Clock, LongCountsAndPartitions) {
    for (const auto frequency : {4000u, 10000u, 30000u, 44100u, 96000u}) {
        Source source;
        source.record = false;
        SampleClock clock(frequency, {0.0}, source);
        constexpr std::uint64_t total = 1000007;
        std::vector<float> buffer(SampleClock::MaximumBlock);
        for (std::uint64_t n = 0; n < total;) {
            const auto count = std::min<std::uint64_t>(total - n, buffer.size());
            clock.render(buffer.data(), count);
            n += count;
            EXPECT_EQ(clock.counts().ticks, n * frequency / rate);
            EXPECT_EQ(source.starts, (n * frequency + rate - 1) / rate);
        }
        EXPECT_EQ(source.samples, total);
        std::vector<float> expected(1009), actual(1009);
        Source first, second;
        SampleClock a(frequency, {0.0}, first), b(frequency, {0.0}, second);
        a.render(expected.data(), expected.size());
        const std::array<std::size_t, 5> parts{7, 251, 31, 1, 503};
        std::size_t n = 0, index = 0;
        while (n < actual.size()) {
            const auto count = std::min(parts[index++ % parts.size()], actual.size() - n);
            b.render(actual.data() + n, count);
            n += count;
        }
        EXPECT_EQ(actual, expected);
        EXPECT_EQ(second.events, first.events);
        Source single;
        SampleClock c(frequency, {0.0}, single);
        for (auto &value : actual) c.render(&value, 1);
        EXPECT_EQ(actual, expected);
        EXPECT_EQ(single.events, first.events);
    }
}

TEST(Clock, DesktopRetimingPreservesPartialTicksAndPartitionedTimeline) {
    struct RetimedSource : ClockSource {
        std::uint64_t ticks = 0;
        std::vector<double> inputs;
        void latch() override {}
        bool tick(double *out, std::size_t capacity) override {
            if (out == nullptr || capacity == 0) return false;
            out[0] = static_cast<double>(++ticks % 257);
            return true;
        }
        bool native(AcousticExcitation input, std::int16_t &out) override {
            inputs.push_back(input.data[0]);
            out = static_cast<std::int16_t>(std::lround(input.data[0]));
            return true;
        }
    };
    const std::vector<std::pair<std::size_t, std::uint32_t>> edges{
        {0, 10000}, {1, 400000}, {13, 400}, {1001, 44100}, {1049, 30000},
        {1100, 10001}, {4000, 96000}, {7000, 400000}, {9001, 400}, {11000, 10000}};
    const auto run = [&](std::size_t partition, bool checkReference) {
        RetimedSource source;
        SampleClock clock(10000, {0.0}, source, SampleClock::Profile::Desktop);
        std::vector<float> pcm(12001);
        std::uint32_t frequency = 10000, pending = 0;
        std::uint64_t ticks = 0;
        long double start = 0, end = 1.0L / frequency;
        for (std::size_t e = 0; e < edges.size(); ++e) {
            // Only the final request takes effect. Zero-sized calls cannot consume it.
            clock.requestDesktopFrequency(12345);
            clock.render(nullptr, 0);
            clock.requestDesktopFrequency(edges[e].second);
            pending = edges[e].second == frequency ? 0 : edges[e].second;
            const auto stop = e + 1 < edges.size() ? edges[e + 1].first : pcm.size();
            for (auto n = edges[e].first; n < stop;) {
                const auto count = std::min(partition, stop - n);
                clock.render(pcm.data() + n, count);
                if (checkReference) for (auto frame = n; frame < n + count; ++frame) {
                    const long double left = static_cast<long double>(frame) / rate;
                    const long double right = static_cast<long double>(frame + 1) / rate;
                    if (std::abs(left - start) < 1e-14L && pending) {
                        frequency = pending; pending = 0; end = start + 1.0L / frequency;
                    }
                    const auto fraction = (left - start) / (end - start);
                    const auto previous = ticks ? (ticks - 1) % 257 : 0;
                    const auto current = ticks % 257;
                    EXPECT_NEAR(source.inputs[frame], static_cast<double>((1 - fraction) * previous + fraction * current), 1e-7);
                    while (end <= right + 1e-14L) {
                        ++ticks; start = end;
                        if (pending) { frequency = pending; pending = 0; }
                        end = start + 1.0L / frequency;
                    }
                }
                n += count;
                if (checkReference) EXPECT_EQ(clock.counts().ticks, ticks);
            }
        }
        EXPECT_EQ(source.ticks, clock.counts().ticks);
        EXPECT_EQ(clock.counts().frames, pcm.size());
        return std::make_pair(pcm, source.inputs);
    };
    const auto expected = run(1, true);
    EXPECT_EQ(run(257, false), expected);
    EXPECT_EQ(run(SampleClock::MaximumBlock, false), expected);
    Source source;
    SampleClock publicClock(10000, {0.0}, source);
    EXPECT_THROW(publicClock.requestDesktopFrequency(400), std::invalid_argument);
    SampleClock desktopClock(400, {0.0}, source, SampleClock::Profile::Desktop);
    EXPECT_THROW(desktopClock.requestDesktopFrequency(399), std::invalid_argument);
    EXPECT_THROW(desktopClock.requestDesktopFrequency(400001), std::invalid_argument);
}

TEST(Clock, ZeroInvalidAndCounterCapacityDoNotAdvance) {
    Source source;
    SampleClock clock(44100, {0.0}, source);
    EXPECT_THROW(SampleClock(3999, {0.0}, source), std::invalid_argument);
    EXPECT_THROW(SampleClock(96001, {0.0}, source), std::invalid_argument);
    EXPECT_THROW(SampleClock(44100, {}, source), std::invalid_argument);
    clock.render(nullptr, 0);
    EXPECT_EQ(source.samples, 0u);
    std::array<float, 8> output;
    output.fill(42);
    EXPECT_THROW(clock.render(nullptr, 1), std::invalid_argument);
    EXPECT_THROW(clock.render(output.data(), SampleClock::MaximumBlock + 1), std::invalid_argument);
    EXPECT_FALSE(clock.faulted());
    ClockTestAccess::preload(clock, {UINT64_MAX - 2, UINT64_MAX - 2, UINT64_MAX - 2}, 0);
    EXPECT_THROW(clock.render(output.data(), 3), std::overflow_error);
    EXPECT_EQ(source.samples, 0u);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float v) { return v == 42; }));
    clock.render(output.data(), 2);
    EXPECT_EQ(clock.counts().frames, UINT64_MAX);
    EXPECT_EQ(clock.counts().ticks, UINT64_MAX);
    clock.render(nullptr, 0);
    EXPECT_THROW(clock.render(output.data(), 1), std::overflow_error);
}

TEST(Clock, FaultsPublishOnlyCompletedIntervals) {
    for (int failure = 0; failure < 3; ++failure) {
        Source source;
        if (failure == 0) source.failTick = 4;
        if (failure == 1) source.nonfiniteTick = 4;
        if (failure == 2) source.failSample = 3;
        SampleClock clock(44100, {0.0}, source);
        std::array<float, 10> output;
        output.fill(42);
        EXPECT_THROW(clock.render(output.data(), output.size()), SimulationFault);
        EXPECT_EQ(clock.lastWritten(), 3u);
        EXPECT_EQ(clock.counts().frames, 3u);
        EXPECT_EQ(clock.counts().ticks, 3u);
        EXPECT_EQ(clock.counts().nativeFrames, 3u);
        for (std::size_t i = 3; i < output.size(); ++i) EXPECT_EQ(output[i], 42);
        EXPECT_TRUE(clock.faulted());
        EXPECT_THROW(clock.render(output.data(), 1), SimulationFault);
    }
}

TEST(Clock, NativeDspMatchesLegacyKernelAndRejectsNonfiniteInput) {
    Synthesizer legacy, direct;
    Synthesizer::Parameters p;
    p.inputChannelCount = 2;
    p.inputSampleRate = p.audioSampleRate = 44100;
    p.audioBufferSize = p.inputBufferSize = 8192;
    p.initialAudioParameters.convolution = .7f;
    p.initialAudioParameters.airNoise = .3f;
    legacy.initialize(p);
    direct.initialize(p);
    const std::int16_t impulse[]{32767, 8192, -4096};
    for (int channel = 0; channel < 2; ++channel) {
        legacy.initializeImpulseResponse(impulse, 3, channel == 0 ? 1.0f : .75f, channel);
        direct.initializeImpulseResponse(impulse, 3, channel == 0 ? 1.0f : .75f, channel);
    }
    direct.beginSynchronous();
    std::array<double, 2> input{0, 0};
    std::vector<std::int16_t> expected;
    expected.push_back(direct.renderNativeFrame({input.data(), input.size()}));
    for (int i = 0; i < 4096; ++i) {
        input = {1000 * std::sin(i * .03125), 500 * std::cos(i * .125)};
        legacy.writeInput(input.data());
        expected.push_back(direct.renderNativeFrame({input.data(), input.size()}));
    }
    legacy.endInputBlock();
    legacy.renderAudio();
    std::vector<std::int16_t> actual(expected.size());
    ASSERT_EQ(legacy.readAudioOutput(static_cast<int>(actual.size()), actual.data()), actual.size());
    EXPECT_EQ(actual, expected);
    EXPECT_GT(std::count_if(expected.begin(), expected.end(), [](auto value) { return value != 0; }), 1000);
    auto distinct = expected;
    std::sort(distinct.begin(), distinct.end());
    EXPECT_GT(std::distance(distinct.begin(), std::unique(distinct.begin(), distinct.end())), 128);
    input[0] = std::numeric_limits<double>::infinity();
    EXPECT_THROW(direct.renderNativeFrame({input.data(), input.size()}), SimulationFault);
    EXPECT_THROW(direct.writeInput(input.data()), std::logic_error);
    EXPECT_THROW(direct.startAudioRenderingThread(), std::logic_error);
    auto parameters = direct.getAudioParameters();
    parameters.volume = std::numeric_limits<float>::infinity();
    direct.setAudioParameters(parameters);
    input[0] = 0;
    EXPECT_THROW(direct.renderNativeFrame({input.data(), input.size()}), SimulationFault);
}

PreparedEngine fixture() {
    const auto *root = std::getenv("ENGINE_SIM_TEST_SOURCE");
    if (root == nullptr) throw std::runtime_error("missing test source");
    const auto path = std::filesystem::u8path(root) / "examples/runtime/fixture";
    return prepareEngine({path / "main.mr", path / "library", path});
}

std::array<double, 5> physical(Simulator &sim) {
    auto *engine = sim.getEngine();
    return {engine->getCrankshaft(0)->m_body.theta, engine->getCrankshaft(0)->m_body.v_theta,
            engine->getChamber(0)->m_system.pressure(), engine->getChamber(0)->m_system.volume(),
            engine->getThrottlePlateAngle()};
}

TEST(Clock, RealMechanicsMatchLegacyTicksAndPartitionedPulls) {
    auto old = fixture();
    auto fresh = fixture();
    auto &a = *old.session->simulator;
    auto &b = *fresh.session->simulator;
    auto control = [](Simulator &sim) {
        sim.getEngine()->setSpeedControl(.25);
        sim.getEngine()->getIgnitionModule()->m_enabled = true;
        sim.m_starterMotor.m_enabled = true;
    };
    control(a);
    std::vector<std::array<double, 5>> expected, actual;
    engine_sim_test::LegacyFrame legacy(a, old.session->synthesis);
    legacy.startFixedFrame(a.getSimulationFrequency());
    while (legacy.simulateStep()) expected.push_back(physical(a));
    legacy.endFrame();
    PullEngine pull(b, fresh.session->synthesis, [&] { control(b); });
    pull.setTickObserver([&](const Simulator &) { actual.push_back(physical(b)); });
    std::vector<float> pcm(44100);
    pull.render(pcm.data(), pcm.size());
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(pull.counts().ticks, expected.size());
    auto other = fixture();
    auto &c = *other.session->simulator;
    PullEngine partitioned(c, other.session->synthesis, [&] { control(c); });
    std::vector<float> split(pcm.size());
    for (std::size_t i = 0; i < split.size();) {
        const auto count = std::min<std::size_t>(257, split.size() - i);
        partitioned.render(split.data() + i, count);
        i += count;
    }
    EXPECT_EQ(split, pcm);
    EXPECT_EQ(physical(c), physical(b));
    EXPECT_EQ(fresh.session->synthesis.getBufferStatistics().queuedOutput, 0u);
}

TEST(Clock, CheckedSolverAndInvalidBodyFaultBeforeUnsafeUse) {
    CheckedSolver solver;
    auto prepared = fixture();
    auto &sim = *prepared.session->simulator;
    solver.m_maxIterations = 0;
    auto *system = dynamic_cast<atg_scs::OptimizedNsvRigidBodySystem *>(sim.getSystem());
    ASSERT_NE(system, nullptr);
    system->initialize(&solver);
    PullEngine pull(sim, prepared.session->synthesis, {});
    std::array<float, 32> pcm;
    pcm.fill(42);
    EXPECT_THROW(pull.render(pcm.data(), pcm.size()), SimulationFault);
    for (std::size_t i = pull.lastWritten(); i < pcm.size(); ++i) EXPECT_EQ(pcm[i], 42);
    auto invalid = fixture();
    PullEngine bad(*invalid.session->simulator, invalid.session->synthesis, {});
    invalid.session->definition.engine->getCrankshaft(0)->m_body.theta = std::numeric_limits<double>::infinity();
    EXPECT_THROW(bad.render(pcm.data(), pcm.size()), SimulationFault);
}
}
