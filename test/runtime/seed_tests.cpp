#include "prepared_engine.h"
#include "random_stream.h"

#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

#include "random_vectors.h"

#include "legacy_access.h"

namespace {
namespace fs = std::filesystem;
using engine_sim::PreparedEngine;
using engine_sim::prepareEngine;

TEST(Seed, PublishedDomainAndIndexVectors) {
    const Synthesizer::Parameters legacyParameters{2, 2048, 4096, 30000, 44100, {}};
    EXPECT_EQ(legacyParameters.inputChannelCount, 2);
    EXPECT_EQ(legacyParameters.inputSampleRate, 30000);
    EXPECT_EQ(legacyParameters.seed, 1u);
    for (const auto &test : randomVectors) {
        engine_sim::RandomStream stream(test.seed, test.domain, test.index);
        for (const auto word : test.words) EXPECT_EQ(stream.next(), word);
        stream.reset(test.seed, test.domain, test.index);
        for (const auto word : test.words) {
            const auto value = stream.uniform();
            EXPECT_DOUBLE_EQ(value, static_cast<double>(word >> 11) * 0x1.0p-53);
            EXPECT_GE(value, 0);
            EXPECT_LT(value, 1);
        }
    }
}

struct Trace {
    std::vector<std::int16_t> pcm;
    std::vector<double> mechanics;
    bool operator==(const Trace &other) const { return pcm == other.pcm && mechanics == other.mechanics; }
};

class SeedEngines : public testing::Test {
protected:
    void SetUp() override {
        const auto *source = std::getenv("ENGINE_SIM_TEST_SOURCE");
        ASSERT_NE(source, nullptr);
        const auto root = fs::current_path() / testing::UnitTest::GetInstance()->current_test_info()->name();
        for (int i = 0; i < 2; ++i) {
            directories[i] = root / std::to_string(i);
            fs::create_directories(directories[i]);
            for (const char *part : {"es", "assets"}) {
                fs::copy(fs::u8path(source) / part, directories[i] / part,
                    fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            }
            std::ofstream entry(directories[i] / "entry.mr");
            entry << "import \"engine_sim.mr\"\nimport \"engines/"
                << (i == 0 ? "atg-video-1/02_kohler_ch750.mr" : "atg-video-2/07_gm_ls.mr")
                << "\"\nmain()\n";
            ASSERT_TRUE(entry);
        }
    }
    PreparedEngine prepare(int index, std::uint64_t seed) {
        return prepareEngine({directories[index] / "entry.mr", directories[index] / "es",
            directories[index] / "assets"}, {0, ES_FREE, 0, seed});
    }
    void block(PreparedEngine &prepared, Trace &trace, int index) {
        auto &sim = *prepared.session->simulator;
        auto &engine = *prepared.session->definition.engine;
        engine.setSpeedControl(index < 60 ? 0.15 : 0.4);
        engine.getIgnitionModule()->m_enabled = true;
        sim.m_starterMotor.m_enabled = index < 100;
        const int ticks = sim.getSimulationFrequency() / 100;
        engine_sim_test::LegacyAccess::begin(sim, ticks);
        while (sim.simulateStep()) {}
        sim.endFrame();
        auto &synth = sim.synthesizer();
        synth.renderAudio();
        const auto frames = static_cast<int>(synth.getBufferStatistics().queuedOutput);
        std::vector<std::int16_t> pcm(frames);
        ASSERT_EQ(synth.readAudioOutput(frames, pcm.data()), frames);
        trace.pcm.insert(trace.pcm.end(), pcm.begin(), pcm.end());
        trace.mechanics.insert(trace.mechanics.end(), {engine.getCrankshaft(0)->m_body.v_theta,
            engine.getCrankshaft(0)->getCycleAngle(), engine.getChamber(0)->m_system.pressure()});
    }
    Trace run(PreparedEngine &engine) {
        Trace trace;
        for (int index = 0; index < 160; ++index) block(engine, trace, index);
        return trace;
    }
    std::array<fs::path, 2> directories;
};

TEST_F(SeedEngines, InterleavedAndParallelEnginesMatchIsolatedRuns) {
    auto a = prepare(0, 0);
    auto b = prepare(1, UINT64_MAX);
    const auto expectedA = run(a);
    const auto expectedB = run(b);
    ASSERT_GT(expectedA.pcm.size(), 70000u);
    ASSERT_GT(expectedB.pcm.size(), 70000u);
    EXPECT_NE(expectedA.mechanics.back(), 0);
    for (const bool reverse : {false, true}) {
        a = prepare(0, 0);
        b = prepare(1, UINT64_MAX);
        Trace actualA, actualB;
        for (int i = 0; i < 160; ++i) {
            if (reverse) { block(b, actualB, i); block(a, actualA, i); }
            else { block(a, actualA, i); block(b, actualB, i); }
        }
        EXPECT_TRUE(actualA == expectedA);
        EXPECT_TRUE(actualB == expectedB);
    }
    a = prepare(0, 0);
    b = prepare(1, UINT64_MAX);
    auto resultA = std::async(std::launch::async, [&] { return run(a); });
    auto resultB = std::async(std::launch::async, [&] { return run(b); });
    EXPECT_TRUE(resultA.get() == expectedA);
    EXPECT_TRUE(resultB.get() == expectedB);
    a = prepare(0, 0);
    std::promise<void> halfway, resume;
    auto reachedHalfway = halfway.get_future();
    auto continueRendering = resume.get_future();
    auto rendering = std::async(std::launch::async, [&] {
        Trace trace;
        for (int i = 0; i < 80; ++i) block(a, trace, i);
        halfway.set_value();
        continueRendering.wait();
        for (int i = 80; i < 160; ++i) block(a, trace, i);
        return trace;
    });
    reachedHalfway.wait();
    b = prepare(1, 12);
    b.session.reset();
    resume.set_value();
    EXPECT_TRUE(rendering.get() == expectedA);
}

TEST_F(SeedEngines, HostLibcRandomSequenceSurvivesPreparationAndRendering) {
    constexpr unsigned seed = 4317;
    std::array<int, 16> expected;
    std::srand(seed);
    for (auto &value : expected) value = std::rand();
    std::srand(seed);
    { auto prepared = prepare(0, 1); (void)run(prepared); }
    for (const auto value : expected) EXPECT_EQ(std::rand(), value);
    auto first = prepare(0, 1);
    auto second = prepare(0, 2);
    EXPECT_NE(run(first).pcm, run(second).pcm);
}
}
