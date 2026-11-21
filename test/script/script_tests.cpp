#include <gtest/gtest.h>

#include "compiler.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>

namespace {
namespace fs = std::filesystem;

class ScriptTests : public testing::Test {
protected:
    void SetUp() override {
        const char *resourcePath = std::getenv("ENGINE_SIM_TEST_RESOURCES");
        const char *fixturePath = std::getenv("ENGINE_SIM_TEST_SCRIPTS");
        ASSERT_NE(resourcePath, nullptr);
        ASSERT_NE(fixturePath, nullptr);
        resources = fs::path(resourcePath);
        fixtures = fs::path(fixturePath);
        ASSERT_TRUE(resources.is_absolute());
        ASSERT_TRUE(fixtures.is_absolute());
    }

    bool compile(const fs::path &entry, es_script::Compiler::Output *output = nullptr) {
        es_script::Compiler compiler;
        compiler.initialize({(resources / "es").string()});
        const bool compiled = compiler.compile(entry.string(), diagnostics);
        if (compiled && output != nullptr) {
            *output = compiler.execute();
        }
        compiler.destroy();
        return compiled;
    }

    fs::path resources;
    fs::path fixtures;
    std::ostringstream diagnostics;
};

void destroyEngine(Engine *engine) {
    engine->destroy();
    delete engine;
}

TEST_F(ScriptTests, DefaultScriptProducesTheExpectedEngine) {
    es_script::Compiler::Output output;
    ASSERT_TRUE(compile(resources / "assets" / "main.mr", &output)) << diagnostics.str();
    std::unique_ptr<Engine, decltype(&destroyEngine)> engine(output.engine.release(), destroyEngine);
    std::unique_ptr<Vehicle> vehicle(std::move(output.vehicle));
    std::unique_ptr<Transmission> transmission(std::move(output.transmission));
    ASSERT_NE(engine.get(), nullptr);
    ASSERT_NE(vehicle.get(), nullptr);
    ASSERT_NE(transmission.get(), nullptr);
    EXPECT_EQ(engine->getName(), "Subaru EJ25");
    EXPECT_EQ(engine->getCylinderCount(), 4);
    EXPECT_EQ(engine->getCylinderBankCount(), 2);
    EXPECT_EQ(engine->getExhaustSystemCount(), 1);
    EXPECT_DOUBLE_EQ(engine->getSimulationFrequency(), 20000.0);
    EXPECT_GT(vehicle->getMass(), 0.0);
    EXPECT_TRUE(diagnostics.str().empty()) << diagnostics.str();
}

TEST_F(ScriptTests, ResolvesLibraryThroughNestedImports) {
    ASSERT_TRUE(compile(fixtures / "library.mr")) << diagnostics.str();
    EXPECT_TRUE(diagnostics.str().empty()) << diagnostics.str();
}

TEST_F(ScriptTests, ReportsMissingEntry) {
    const fs::path missing = fixtures / "nonexistent_entry.mr";
    es_script::Compiler::Output output;
    EXPECT_FALSE(compile(missing, &output));
    EXPECT_EQ(output.engine, nullptr);
    EXPECT_NE(diagnostics.str().find(missing.string()), std::string::npos);
}

TEST_F(ScriptTests, ReportsMissingImport) {
    es_script::Compiler::Output output;
    EXPECT_FALSE(compile(fixtures / "missing-import.mr", &output));
    EXPECT_EQ(output.engine, nullptr);
    EXPECT_NE(diagnostics.str().find("missing-import(1): error"), std::string::npos)
        << diagnostics.str();
}

TEST_F(ScriptTests, ReportsUnknownNode) {
    es_script::Compiler::Output output;
    EXPECT_FALSE(compile(fixtures / "unknown-node.mr", &output));
    EXPECT_EQ(output.engine, nullptr);
    EXPECT_NE(diagnostics.str().find("unknown-node(2): error R0010"), std::string::npos)
        << diagnostics.str();
}
}
