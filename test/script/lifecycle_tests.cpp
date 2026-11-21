#include <gtest/gtest.h>
#include "compiler.h"
#include "piston_engine_simulator.h"
#include "standard_valvetrain.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

namespace {
namespace fs = std::filesystem;

struct CountedResource {
    inline static int alive = 0;
    CountedResource() { ++alive; }
    ~CountedResource() { --alive; }
};

TEST(Lifecycle, EmptyAndRepeatedRelease) {
    Engine engine;
    engine.createResource<CountedResource>();
    EXPECT_EQ(CountedResource::alive, 1);
    engine.destroy();
    engine.destroy();
    EXPECT_EQ(CountedResource::alive, 0);
    PistonEngineSimulator simulator;
    simulator.releaseSimulation();
    simulator.releaseSimulation();
    Synthesizer synth;
    synth.destroy();
    synth.destroy();
}

TEST(Lifecycle, PartialGraphReleaseAndReinitialize) {
    Engine engine;
    Engine::Parameters params{};
    params.cylinderBanks = 2;
    params.cylinderCount = 9;
    params.crankshaftCount = 1;
    params.intakeCount = params.exhaustSystemCount = 1;
    for (int i = 0; i < 3; ++i) {
        engine.initialize(params);
        auto *function = engine.createResource<Function>();
        function->initialize(4, 1);
        auto *cam = engine.createResource<Camshaft>();
        Camshaft::Parameters camParams{};
        camParams.lobes = 9;
        cam->initialize(camParams);
        engine.createResource<CountedResource>();
        engine.destroy();
        EXPECT_EQ(CountedResource::alive, 0);
        EXPECT_EQ(engine.getCylinderCount(), 0);
    }
}

TEST(Lifecycle, BothSolverOwnersRelease) {
    for (const auto type : {Simulator::SystemType::NsvOptimized, Simulator::SystemType::Generic}) {
        PistonEngineSimulator simulator;
        Simulator::Parameters params;
        params.systemType = type;
        simulator.initialize(params);
        simulator.releaseSimulation();
        simulator.releaseSimulation();
    }
}

TEST(Lifecycle, SynthesizerReinitializationAndIdleWorkerStop) {
    Synthesizer synth;
    Synthesizer::Parameters params;
    params.inputBufferSize = params.audioBufferSize = 44100;
    params.initialAudioParameters.inputSampleNoise = 0;
    params.initialAudioParameters.airNoise = 0;
    constexpr std::int16_t taps[]{32767, -8192, 2048};
    std::array<std::int16_t, 1024> initial{};
    int initialCount = 0;
    for (int iteration = 0; iteration < 25; ++iteration) {
        synth.initialize(params);
        EXPECT_EQ(synth.getLatency(), 0);
        synth.startAudioRenderingThread();
        synth.endAudioRenderingThread();
        synth.initializeImpulseResponse(taps, 3, 1, 0);
        for (int tick = 0; tick < 100; ++tick) {
            const double signal = 1000 * std::sin(tick * 0.03125);
            synth.writeInput(&signal);
        }
        synth.endInputBlock();
        synth.renderAudio();
        std::array<std::int16_t, 1024> current{};
        const int count = synth.readAudioOutput(current.size(), current.data());
        ASSERT_GT(count, 0);
        if (iteration == 0) {
            initial = current;
            initialCount = count;
        }
        EXPECT_EQ(count, initialCount);
        EXPECT_EQ(current, initial);
        const double input = 1000;
        for (int tick = 0; tick < 100; ++tick) synth.writeInput(&input);
        synth.endInputBlock();
        synth.startAudioRenderingThread();
        synth.waitProcessed();
        synth.endAudioRenderingThread();
        EXPECT_GT(synth.readAudioOutput(current.size(), current.data()), 0);
        synth.destroy();
        synth.destroy();
        EXPECT_EQ(synth.m_inputChannels, nullptr);
        EXPECT_EQ(synth.m_filters, nullptr);
        EXPECT_EQ(synth.m_thread, nullptr);
    }
}

TEST(Lifecycle, RepeatedKohlerAndNineCylinderStartStop) {
    const char *resourcePath = std::getenv("ENGINE_SIM_TEST_RESOURCES");
    ASSERT_NE(resourcePath, nullptr);
    const fs::path resources(resourcePath);
    const fs::path entry = fs::current_path() / "lifecycle-entry.mr";
    for (int cycle = 0; cycle < 25; ++cycle) {
        const bool radial = cycle % 2 != 0;
        {
            std::ofstream script(entry);
            script << "import \"engine_sim.mr\"\nimport \"engines/"
                   << (radial ? "atg-video-2/09_radial_9.mr" : "atg-video-1/02_kohler_ch750.mr")
                   << "\"\nmain()\n";
        }
        es_script::Compiler compiler;
        compiler.initialize({(resources / "es").string(), (resources / "assets").string()});
        std::ostringstream diagnostic;
        ASSERT_TRUE(compiler.compile(entry.string(), diagnostic)) << diagnostic.str();
        const auto output = compiler.execute();
        compiler.destroy();
        std::unique_ptr<Engine> engine(output.engine);
        std::unique_ptr<Vehicle> vehicle(output.vehicle);
        std::unique_ptr<Transmission> transmission(output.transmission);
        ASSERT_NE(engine, nullptr);
        ASSERT_NE(vehicle, nullptr);
        ASSERT_NE(transmission, nullptr);
        ASSERT_EQ(engine->getCylinderCount(), radial ? 9 : 2);
        engine->calculateDisplacement();
        std::unique_ptr<Simulator> simulator(engine->createSimulator(vehicle.get(), transmission.get()));
        simulator->setSimulationFrequency(static_cast<int>(engine->getSimulationFrequency()));
        constexpr std::int16_t impulse[]{32767};
        for (int i = 0; i < engine->getExhaustSystemCount(); ++i) {
            simulator->synthesizer().initializeImpulseResponse(impulse, 1, 1, i);
        }
        simulator->startFrame(0);
        simulator->endFrame();
        for (int i = 0; i < engine->getIntakeCount(); ++i) {
            EXPECT_EQ(engine->getIntake(i)->m_flowRate, 0);
        }
        simulator->m_starterMotor.m_enabled = true;
        engine->getIgnitionModule()->m_enabled = true;
        engine->setSpeedControl(0.1);
        int totalTicks = 0;
        for (int block = 0; block < 50; ++block) {
            simulator->setTargetSynthesizerLatency(simulator->getSynthesizerInputLatency());
            simulator->startFrame(0.01);
            while (simulator->simulateStep()) ++totalTicks;
            simulator->endFrame();
            simulator->synthesizer().renderAudio();
            std::array<std::int16_t, 2048> audio{};
            EXPECT_GT(simulator->readAudioOutput(audio.size(), audio.data()), 0);
            ASSERT_TRUE(std::isfinite(engine->getRpm()));
            for (int i = 0; i < engine->getCylinderCount(); ++i) {
                const auto &gas = engine->getChamber(i)->m_system;
                ASSERT_TRUE(std::isfinite(gas.pressure()));
                ASSERT_GT(gas.pressure(), 0);
                ASSERT_GT(gas.volume(), 0);
                ASSERT_GE(gas.n(), 0);
            }
        }
        EXPECT_EQ(totalTicks, simulator->getSimulationFrequency() / 2);
        simulator->releaseSimulation();
        simulator->releaseSimulation();
        simulator.reset();
        engine->destroy();
        engine->destroy();

        es_script::Compiler rejected;
        rejected.initialize({(resources / "es").string()});
        EXPECT_FALSE(rejected.compile((resources / "missing-lifecycle-entry.mr").string(), diagnostic));
        rejected.destroy();
    }
    RecordProperty("reload_cycles", 25);
    RecordProperty("radial_cylinders", 9);
    fs::remove(entry);
}
}
