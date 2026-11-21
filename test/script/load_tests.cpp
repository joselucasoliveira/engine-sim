#include <gtest/gtest.h>
#include "compiler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace {
namespace fs = std::filesystem;
class Load : public testing::Test {
protected:
    void SetUp() override {
        const char *path = std::getenv("ENGINE_SIM_TEST_RESOURCES");
        ASSERT_NE(path, nullptr);
        resources = fs::canonical(fs::path(path));
        const auto relative = fs::canonical(fs::current_path()).lexically_relative(resources);
        ASSERT_TRUE(relative.empty() || *relative.begin() == "..") << "Load fixtures require an external working directory";
        compiler.initialize({(resources / "es").string(), (resources / "assets").string()});
    }
    fs::path write(const std::string &name, const std::string &text) {
        const auto entry = fs::current_path() / (std::string(testing::UnitTest::GetInstance()->current_test_info()->name()) + "-" + name + ".mr");
        std::ofstream file(entry);
        file << text;
        if (!file) throw std::runtime_error("Unable to write external test fixture");
        return entry;
    }
    EngineDefinition execute(const fs::path &entry) {
        std::ostringstream diagnostics;
        if (!compiler.compile(entry.string(), diagnostics)) throw std::runtime_error(diagnostics.str());
        return compiler.execute();
    }
    template <typename F> void expectError(F operation, const std::string &stage, const fs::path &path,
            const std::string &fragment = "") {
        try { operation(); ADD_FAILURE() << "Expected a structured load error"; }
        catch (const EngineLoadError &error) {
            EXPECT_EQ(error.diagnostic.stage, stage);
            EXPECT_EQ(error.diagnostic.path, path.string());
            EXPECT_NE(error.diagnostic.message.find(fragment), std::string::npos) << error.what();
        }
    }
    static std::vector<int16_t> unitImpulse(const ImpulseResponse &) { return {INT16_MAX}; }
    std::unique_ptr<EngineSession> validSession() {
        return EngineSession::prepare(execute(resources / "assets/main.mr"), unitImpulse);
    }
    fs::path resources;
    es_script::Compiler compiler;
};

TEST_F(Load, FreshOutputAndSingleExecution) {
    auto first = execute(resources / "assets/main.mr");
    ASSERT_NE(first.engine, nullptr);
    auto *original = first.engine.get();
    const auto empty = write("empty", "import \"engine_sim.mr\"\n");
    expectError([&] { execute(empty); }, "graph", empty, "did not select an engine");
    EXPECT_EQ(first.engine.get(), original);
    EXPECT_EQ(first.engine->getCylinderCount(), 4);
    auto second = execute(resources / "assets/main.mr");
    EXPECT_NE(second.engine.get(), original);
    EXPECT_NE(second.vehicle.get(), first.vehicle.get());
    expectError([&] { compiler.execute(); }, "execute", resources / "assets/main.mr", "No unexecuted");
    EXPECT_THROW(es_script::Compiler::output(), std::logic_error);
    es_script::Compiler other;
    other.initialize({(resources / "es").string()});
    std::ostringstream diagnostics;
    ASSERT_TRUE(other.compile(empty.string(), diagnostics)) << diagnostics.str();
    expectError([&] { other.execute(); }, "graph", empty, "did not select");
    EXPECT_EQ(first.engine.get(), original);
    other.destroy();
    other.destroy();
}

TEST_F(Load, SameCompilerRecoversAcrossFailures) {
    const auto empty = write("empty", "import \"engine_sim.mr\"\n");
    const auto missingIgnition = write("incomplete", "import \"engine_sim.mr\"\nset_engine(engine())\n");
    const auto nonfinite = write("nonfinite", "import \"engine_sim.mr\"\nset_engine(engine(simulation_frequency: 1.0 / 0.0))\n");
    const auto syntax = write("syntax", "import \"engine_sim.mr\"\nset_engine(\n");
    const auto missingInput = write("input", "import \"engine_sim.mr\"\nset_engine()\n");
    const std::array<fs::path,5> discarded{
        write("discarded-label", "invalid !!!\n"),
        write("discarded-expression", "unknown(1 + (2 * 3) + !!!\n"),
        write("discarded-ports", "! { input x; }\n"),
        write("discarded-body", "! { input x; unknown() }\n"),
        write("recovered-connection", "unknown(! ! !)\n")};
    for (int repeat = 0; repeat < 5; ++repeat) {
        auto valid = execute(resources / "assets/main.mr");
        ASSERT_NE(valid.engine, nullptr);
        expectError([&] { execute(empty); }, "graph", empty);
        expectError([&] { execute(missingIgnition); }, "graph", missingIgnition, "ignition");
        expectError([&] { execute(nonfinite); }, "graph", nonfinite, "finite");
        for (const auto &entry : {syntax, missingInput, fs::current_path() / "absent-load-entry.mr"}) {
            std::ostringstream diagnostics;
            EXPECT_FALSE(compiler.compile(entry.string(), diagnostics));
            EXPECT_EQ(compiler.getLastError().stage, "compile");
            EXPECT_EQ(compiler.getLastError().path, entry.string());
            EXPECT_FALSE(diagnostics.str().empty());
            expectError([&] { compiler.execute(); }, "execute", entry);
        }
        for (const auto &entry:discarded) {
            std::ostringstream diagnostics;
            EXPECT_FALSE(compiler.compile(entry.string(),diagnostics));
            EXPECT_FALSE(diagnostics.str().empty());
            expectError([&]{compiler.execute();},"execute",entry);
        }
        auto recovered = execute(resources / "assets/main.mr");
        EXPECT_EQ(recovered.engine->getName(), valid.engine->getName());
        EXPECT_EQ(recovered.engine->getCylinderCount(), 4);
    }
    EXPECT_FALSE(fs::exists(fs::current_path() / "error_log.log"));
}

TEST_F(Load, RuntimeActionFailureIsPropagated) {
    const auto entry = write("runtime", R"(
import "engine_sim.mr"
function flow()
camshaft cam(lobe_profile: flow)
cylinder_head head(intake_camshaft: cam, exhaust_camshaft: cam)
cylinder_bank bank(bore: 0.1, deck_height: 0.2)
bank.set_cylinder_head(head)
bank.set_cylinder_head(head)
)");
    expectError([&] { execute(entry); }, "execute", entry, "already has a head");
    EXPECT_EQ(compiler.getLastError().stage, "execute");
    auto recovered = execute(resources / "assets/main.mr");
    EXPECT_NE(recovered.engine, nullptr);
}

TEST_F(Load, PartialAndRepeatedDefinitionsReleaseTheirOwners) {
    const std::string prelude = "import \"engine_sim.mr\"\nimport \"engines/atg-video-2/01_subaru_ej25_eh.mr\"\n";
    const auto failing = write("partial", prelude + "main()\nharmonic_cam_lobe(duration_at_50_thou: 0.01, gamma: 0.0)\n");
    const auto repeated = write("repeated", prelude + "main()\nmain()\n");
    for (int iteration = 0; iteration < 5; ++iteration) {
        expectError([&] { execute(failing); }, "graph", failing, "cam gamma");
        auto definition = execute(repeated);
        EXPECT_EQ(definition.engine->getCylinderCount(), 4);
        EXPECT_NE(definition.vehicle, nullptr);
        EXPECT_NE(definition.transmission, nullptr);
    }
}

TEST_F(Load, GraphCyclesReferencesAndPrimitivesAreRejected) {
    std::ifstream file(resources / "assets/engines/atg-video-2/09_radial_9.mr");
    ASSERT_TRUE(file.good());
    std::ostringstream sourceStream;
    sourceStream << file.rdbuf();
    ASSERT_FALSE(sourceStream.str().empty());
    const std::string source = sourceStream.str();
    const auto mutate = [&](const std::string &name, const std::string &from, const std::string &to) {
        auto text = source;
        const std::string localImport = "import \"radial.mr\"";
        const auto importOffset = text.find(localImport);
        if (importOffset == std::string::npos) throw std::runtime_error("Missing declaration-relative fixture import");
        text.replace(importOffset, localImport.size(), "import \"" + (resources / "assets/engines/atg-video-2/radial.mr").generic_string() + "\"");
        const auto location = text.find(from);
        if (location == std::string::npos) throw std::runtime_error("Required negative fixture anchor missing");
        text.replace(location, from.size(), to);
        return write(name, text + "\nmain()\n");
    };
    const auto cycle = mutate("cycle", "rod_journal: rj0,", "rod_journal: sj1,");
    expectError([&] { execute(cycle); }, "graph", cycle, "cycle");
    const auto bore = mutate("bore", "bore: bore,", "bore: 0.0,");
    expectError([&] { execute(bore); }, "graph", bore, "bore");
    const auto foreign = mutate("foreign", "engine.add_ignition_module(",
        "crankshaft foreign()\nforeign.add_rod_journal(rj0)\nengine.add_ignition_module(");
    expectError([&] { execute(foreign); }, "graph", foreign, "ownership");
    auto valid = execute(resources / "assets/main.mr");
    EXPECT_EQ(valid.engine->getCylinderCount(), 4);
}

TEST_F(Load, FailedCandidatePreservesRunningSession) {
    auto active = validSession();
    auto *original = active.get();
    auto *engine = active->definition.engine.get();
    active->simulator->setSimulationFrequency(12345);
    active->simulator->startAudioRenderingThread();
    auto &synth = active->simulator->synthesizer();
    const auto publish = [&] {
        for (int i = 0; i < 32; ++i) { const double value = i * 100; synth.writeInput(&value); }
        synth.endInputBlock();
        synth.waitProcessed();
    };
    active->simulator->startFrame(0.001);
    while (active->simulator->simulateStep()) {}
    active->simulator->endFrame();
    synth.waitProcessed();
    ASSERT_GT(active->simulator->getFrameIterationCount(), 0);
    const auto oldIteration = active->simulator->getCurrentIteration();
    const auto oldPressure = engine->getChamber(0)->m_system.pressure();
    publish();
    const auto progress = synth.getBufferStatistics().completedGeneration;
    const auto settings = active->definition.applicationSettings.powerUnits;
    auto replacement = execute(resources / "assets/main.mr");
    replacement.applicationSettings.powerUnits = "rejected-units";
    const auto attemptedPath = replacement.sourcePath;
    expectError([&] {
        auto candidate = EngineSession::prepare(std::move(replacement), [](const ImpulseResponse &) -> std::vector<int16_t> {
            throw EngineLoadError({"resources", "generated-missing.wav", "Missing impulse file"});
        });
        active.swap(candidate);
    }, "resources", fs::path("generated-missing.wav"));
    EXPECT_EQ(active.get(), original);
    EXPECT_EQ(active->definition.engine.get(), engine);
    EXPECT_EQ(active->simulator->getSimulationFrequency(), 12345);
    EXPECT_EQ(active->simulator->getCurrentIteration(), oldIteration);
    EXPECT_DOUBLE_EQ(engine->getChamber(0)->m_system.pressure(), oldPressure);
    EXPECT_DOUBLE_EQ(active->simulator->getTimestep(), 1.0 / 12345);
    EXPECT_EQ(active->definition.applicationSettings.powerUnits, settings);
    publish();
    EXPECT_EQ(synth.getBufferStatistics().completedGeneration, progress + 1);
    std::array<int16_t, 1024> output;
    EXPECT_GT(synth.readAudioOutput(output.size(), output.data()), 0);
    EXPECT_EQ(active->definition.sourcePath, attemptedPath);
}

TEST_F(Load, RepeatedModelsKeepPcmAndTopology) {
    for (const auto &module : {"engines/atg-video-1/02_kohler_ch750.mr",
            "engines/atg-video-2/07_gm_ls.mr", "engines/atg-video-2/09_radial_9.mr"}) {
        const auto entry = write("replay", "import \"engine_sim.mr\"\nimport \"" + std::string(module) + "\"\nmain()\n");
        std::vector<int16_t> reference;
        std::vector<int> referenceSteps;
        int cylinderCount = 0;
        for (int repetition = 0; repetition < 3; ++repetition) {
            std::srand(1);
            auto session = EngineSession::prepare(execute(entry), unitImpulse);
            auto &engine = *session->definition.engine;
            auto &simulator = *session->simulator;
            const int expectedCylinders = std::string(module).find("kohler") != std::string::npos ? 2
                : std::string(module).find("gm_ls") != std::string::npos ? 8 : 9;
            ASSERT_EQ(engine.getCylinderCount(), expectedCylinders);
            ASSERT_EQ(engine.getExhaustSystemCount(), expectedCylinders == 2 ? 1 : 2);
            ASSERT_EQ(engine.getSimulationFrequency(), expectedCylinders == 2 ? 30000
                : expectedCylinders == 8 ? 10000 : 7500);
            engine.getIgnitionModule()->m_enabled = true;
            simulator.m_starterMotor.m_enabled = true;
            engine.setSpeedControl(0.2);
            std::vector<int16_t> pcm;
            std::vector<int> steps;
            for (int block = 0; block < 20; ++block) {
                simulator.startFrame(0.01);
                while (simulator.simulateStep()) {}
                steps.push_back(simulator.getFrameIterationCount());
                simulator.endFrame();
                auto &synth = simulator.synthesizer();
                synth.renderAudio();
                const auto available = synth.getBufferStatistics().queuedOutput;
                std::vector<int16_t> buffer(available);
                EXPECT_EQ(synth.readAudioOutput(static_cast<int>(available), buffer.data()), available);
                pcm.insert(pcm.end(), buffer.begin(), buffer.end());
                EXPECT_TRUE(std::isfinite(engine.getRpm()));
                EXPECT_GT(engine.getChamber(0)->m_system.pressure(), 0);
                EXPECT_TRUE(std::isfinite(engine.getChamber(0)->m_system.pressure()));
            }
            const auto stem = "replay-" + fs::path(module).stem().string() + "-" + std::to_string(repetition);
            std::ofstream audio(stem + ".pcm", std::ios::binary);
            audio.write(reinterpret_cast<const char *>(pcm.data()), pcm.size() * sizeof(int16_t));
            std::ofstream topology(stem + ".csv");
            topology << std::setprecision(17) << "cylinder,bank,exhaust,pressure_pa,rpm\n";
            for (int cylinder = 0; cylinder < engine.getCylinderCount(); ++cylinder) {
                const auto *piston = engine.getPiston(cylinder);
                const auto bank = piston->getCylinderBank()->getIndex();
                topology << cylinder << ',' << bank << ','
                    << engine.getHead(bank)->getExhaustSystem(piston->getCylinderIndex())->getIndex() << ','
                    << engine.getChamber(cylinder)->m_system.pressure() << ',' << engine.getRpm() << '\n';
            }
            EXPECT_FALSE(pcm.empty());
            EXPECT_TRUE(std::any_of(pcm.begin(), pcm.end(), [](int16_t value) { return value != 0; }));
            if (repetition == 0) {
                reference = pcm;
                referenceSteps = steps;
                cylinderCount = engine.getCylinderCount();
            }
            else {
                EXPECT_EQ(pcm, reference) << module << " repetition " << repetition;
                EXPECT_EQ(steps, referenceSteps);
                EXPECT_EQ(engine.getCylinderCount(), cylinderCount);
            }
        }
    }
}

TEST_F(Load, RepeatedReplacementRetainsSignalAndCleanup) {
    auto active = validSession();
    for (int repeat = 0; repeat < 10; ++repeat) {
        auto candidate = validSession();
        candidate->simulator->startAudioRenderingThread();
        active->simulator->endAudioRenderingThread();
        active.swap(candidate);
        auto &synth = active->simulator->synthesizer();
        for (int i = 0; i < 100; ++i) { const double value = i == 20 ? 8192 : 0; synth.writeInput(&value); }
        synth.endInputBlock();
        synth.waitProcessed();
        std::array<int16_t, 512> output{};
        EXPECT_GT(synth.readAudioOutput(output.size(), output.data()), 0);
        EXPECT_EQ(synth.getDroppedInputSamples(), 0U);
        EXPECT_EQ(active->definition.engine->getCylinderCount(), 4);
    }
}
}
