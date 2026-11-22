#include "prepared_engine.h"
#include "model_validation.h"

#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

namespace engine_sim_capture {
struct FrameAccess {
    static void begin(Simulator &simulator, int ticks) { simulator.beginExactFrame(ticks); }
};
}

namespace {
namespace fs = std::filesystem;
using engine_sim::PreparationConfig;
using engine_sim::PreparedEngine;
using engine_sim::prepareEngine;

class Preparation : public testing::Test {
protected:
    void SetUp() override {
        const auto *source = std::getenv("ENGINE_SIM_TEST_SOURCE");
        ASSERT_NE(source, nullptr);
        resources = fs::u8path(source);
        fixture = fs::current_path() / testing::UnitTest::GetInstance()->current_test_info()->name();
        fs::create_directories(fixture);
        fs::copy(resources / "examples/runtime/fixture", fixture, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        recipe = {fixture / "main.mr", fixture / "library", fixture};
    }
    fs::path write(const std::string &name, const std::string &bytes) {
        const auto path = fixture / name;
        std::ofstream stream(path, std::ios::binary);
        stream << bytes;
        if (!stream) throw std::runtime_error("Cannot write preparation test input");
        return path;
    }
    std::string read(const fs::path &path) {
        std::string bytes(static_cast<std::size_t>(fs::file_size(path)), '\0');
        std::ifstream stream(path, std::ios::binary);
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!stream) throw std::runtime_error("Cannot read preparation test input");
        return bytes;
    }
    template<typename Operation>
    void reject(Operation operation, const std::string &stage, const std::string &fragment = "") {
        try { operation(); FAIL() << "Expected rejected preparation"; }
        catch (const EngineLoadError &error) {
            EXPECT_EQ(error.diagnostic.stage, stage) << error.what();
            EXPECT_NE(error.diagnostic.message.find(fragment), std::string::npos) << error.what();
        }
    }
    int step(PreparedEngine &prepared, int ticks, std::vector<std::int16_t> *capture = nullptr) {
        auto &sim = *prepared.session->simulator;
        engine_sim_capture::FrameAccess::begin(sim, ticks);
        int completed = 0;
        while (sim.simulateStep()) ++completed;
        EXPECT_EQ(completed, ticks);
        sim.endFrame();
        auto &synth = sim.synthesizer();
        synth.renderAudio();
        const auto available = static_cast<int>(synth.getBufferStatistics().queuedOutput);
        std::vector<std::int16_t> samples(available);
        EXPECT_EQ(synth.readAudioOutput(available, samples.data()), available);
        if (capture != nullptr) capture->insert(capture->end(), samples.begin(), samples.end());
        int active = 0;
        for (const auto sample : samples) active += sample != 0;
        return active;
    }
    fs::path resources, fixture;
    es_script::PreparationContext recipe;
};

TEST_F(Preparation, OriginalFixtureFiresAfterStarterRelease) {
    auto prepared = prepareEngine(recipe);
    auto &engine = *prepared.session->definition.engine;
    auto &sim = *prepared.session->simulator;
    EXPECT_EQ(engine.getCylinderCount(), 1);
    EXPECT_EQ(engine.getName(), "Runtime Single");
    EXPECT_EQ(sim.getSimulationFrequency(), 10000);
    EXPECT_DOUBLE_EQ(sim.synthesizer().getInputSampleRate(), 10000);
    EXPECT_NEAR(engine.getDisplacement(), 0.0004021238596594936, 1e-12);
    EXPECT_GT(prepared.session->definition.vehicle->getMass(), 0);
    EXPECT_EQ(prepared.inputs.size(), 10u); // entry, API wrapper, seven library files and impulse
    engine.setSpeedControl(0.3);
    engine.getIgnitionModule()->m_enabled = true;
    int lights = 0, active = 0;
    std::vector<std::int16_t> baseline;
    for (int block = 0; block < 400; ++block) {
        sim.m_starterMotor.m_enabled = block < 125;
        active += step(prepared, 100, &baseline);
        if (engine.getChamber(0)->popLitLastFrame() && block >= 125) ++lights;
    }
    EXPECT_GT(lights, 4);
    EXPECT_GT(active, 10000);
    EXPECT_GT(engine.getRpm(), 300);
    EXPECT_TRUE(std::isfinite(engine.getChamber(0)->m_system.pressure()));
    auto repeated = prepareEngine(recipe);
    auto &second = *repeated.session->definition.engine;
    second.setSpeedControl(0.3);
    second.getIgnitionModule()->m_enabled = true;
    std::vector<std::int16_t> repeatedPcm;
    for (int block = 0; block < 400; ++block) {
        repeated.session->simulator->m_starterMotor.m_enabled = block < 125;
        step(repeated, 100, &repeatedPcm);
    }
    EXPECT_EQ(baseline, repeatedPcm);
    EXPECT_DOUBLE_EQ(engine.getOutputCrankshaft()->m_body.v_theta, second.getOutputCrankshaft()->m_body.v_theta);
    std::ofstream raw(fixture / "startup.s16le", std::ios::binary);
    raw.write(reinterpret_cast<const char *>(baseline.data()),
        static_cast<std::streamsize>(baseline.size() * sizeof(std::int16_t)));
    ASSERT_TRUE(raw);
    std::cout << "fixture post-starter lights=" << lights << " nonzero_frames=" << active
        << " final_rpm=" << engine.getRpm() << '\n';
}

TEST_F(Preparation, StarterOverrideControlsActualColdCrank) {
    auto low = prepareEngine(recipe, {0, ES_FREE, 5});
    auto high = prepareEngine(recipe, {0, ES_FREE, 80});
    EXPECT_DOUBLE_EQ(low.session->simulator->m_starterMotor.m_maxTorque, units::torque(5, units::Nm));
    EXPECT_DOUBLE_EQ(high.session->simulator->m_starterMotor.m_maxTorque, units::torque(80, units::Nm));
    for (auto *prepared : {&low, &high}) {
        prepared->session->simulator->m_starterMotor.m_enabled = true;
        for (int block = 0; block < 10; ++block) step(*prepared, 100);
    }
    EXPECT_GT(high.session->definition.engine->getRpm(), low.session->definition.engine->getRpm());
    const auto defaultTorque = prepareEngine(recipe).session->simulator->m_starterMotor.m_maxTorque;
    EXPECT_DOUBLE_EQ(defaultTorque, units::torque(40, units::Nm));
}

TEST_F(Preparation, NativePresetsRetainTheirDefinitions) {
    struct Preset { const char *path; int cylinders, frequency; };
    for (const auto &preset : {
        Preset{"atg-video-1/02_kohler_ch750.mr", 2, 30000},
        Preset{"atg-video-2/07_gm_ls.mr", 8, 10000},
        Preset{"atg-video-2/09_radial_9.mr", 9, 7500}}) {
        auto entry = write("entry.mr", std::string("import \"engine_sim.mr\"\nimport \"engines/") + preset.path + "\"\nmain()\n");
        auto prepared = prepareEngine({entry, resources / "es", resources / "assets"});
        EXPECT_EQ(prepared.session->definition.engine->getCylinderCount(), preset.cylinders);
        EXPECT_EQ(prepared.session->simulator->getSimulationFrequency(), preset.frequency);
        EXPECT_GT(prepared.session->definition.engine->getDisplacement(), 0);
        step(prepared, preset.frequency / 100);
    }
}

TEST_F(Preparation, ValidatesFrequencyBeforeSimulatorInitialization) {
    const auto original = read(recipe.entry);
    auto text = original;
    const auto offset = text.find("simulation_frequency: 10000");
    ASSERT_NE(offset, std::string::npos);
    text.replace(offset, std::string("simulation_frequency: 10000").size(), "simulation_frequency: 10000.5");
    write("main.mr", text);
    reject([&] { (void)prepareEngine(recipe); }, "model", "integer");
    auto overridden = prepareEngine(recipe, {48000, ES_FREE, 0});
    EXPECT_EQ(overridden.session->simulator->getSimulationFrequency(), 48000);
    EXPECT_DOUBLE_EQ(overridden.session->simulator->synthesizer().getInputSampleRate(), 48000);
    for (const double torque : {-1.0, 10001.0, std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
        reject([&] { (void)prepareEngine(recipe, {0, ES_FREE, torque}); }, "configuration");
    }
    reject([&] { (void)prepareEngine(recipe, {0, ES_DRIVEN, 1}); }, "unsupported");
    reject([&] { (void)prepareEngine(recipe, {3999, ES_FREE, 0}); }, "configuration");
}

TEST_F(Preparation, FailedResourcesAndChangedInputsPreserveCandidateOwner) {
    auto active = prepareEngine(recipe);
    const auto *original = active.session.get();
    const auto wave = read(fixture / "impulse.wav");
    write("impulse.wav", "invalid");
    reject([&] { active = prepareEngine(recipe); }, "resources");
    reject([&] { active = prepareEngine(recipe, {}, &active.inputs); }, "resources");
    EXPECT_EQ(active.session.get(), original);
    fs::remove(fixture / "impulse.wav");
    reject([&] { active = prepareEngine(recipe); }, "resources");
    write("impulse.wav", wave);
    const auto source = read(recipe.entry);
    write("main.mr", "/* changed malformed bytes");
    reject([&] { active = prepareEngine(recipe, {}, &active.inputs); }, "resources", "changed");
    write("main.mr", source);
    auto fresh = prepareEngine(recipe, {}, &active.inputs);
    EXPECT_EQ(fresh.inputs.size(), active.inputs.size());
    step(active, 100);
}

TEST_F(Preparation, RejectsInvalidGraphsBeforeSolverCreation) {
    const auto tooMany = static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1;
    EXPECT_THROW(es_script::modelCount(tooMany, "test"), es_script::ModelError);
    const auto source = read(recipe.entry);
    struct Change { const char *before, *after, *stage; };
    for (const auto &change : {
        Change{"mass: 4.0, flywheel_mass: 4.0", "mass: -1.0, flywheel_mass: 4.0", "graph"},
        Change{"length: 0.150", "length: 0.001", "model"},
        Change{"deck_height: 0.220", "deck_height: 0.01", "model"},
        Change{"motor.add_crankshaft(shaft)", "motor.add_crankshaft(shaft).add_crankshaft(shaft)", "graph"},
        Change{"set_engine(motor)", "", "graph"}}) {
        auto text = source;
        const auto offset = text.find(change.before);
        ASSERT_NE(offset, std::string::npos);
        text.replace(offset, std::string(change.before).size(), change.after);
        write("main.mr", text);
        reject([&] { (void)prepareEngine(recipe); }, change.stage);
    }
}
}
