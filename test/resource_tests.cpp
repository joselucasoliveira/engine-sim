#include <gtest/gtest.h>
#include "resource_paths.h"
#include "compiled_scene.h"

#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {
namespace fs = std::filesystem;
using engine_sim::ResourcePaths;
using engine_sim::ResourceOptions;
using engine_sim::ResourceError;
struct WorkingDirectory {
    fs::path previous = fs::current_path();
    explicit WorkingDirectory(const fs::path &path) { fs::current_path(path); }
    ~WorkingDirectory() { std::error_code error; fs::current_path(previous, error); }
};
class ResourceFixture : public testing::Test {
protected:
    void SetUp() override {
        const auto *value = std::getenv("ENGINE_SIM_TEST_SOURCE");
        ASSERT_NE(value, nullptr);
        source = fs::canonical(fs::u8path(value));
        const auto relative = fs::canonical(fs::current_path()).lexically_relative(source);
        ASSERT_TRUE(relative.empty() || *relative.begin() == "..");
        root = fs::current_path() / (std::string("resource-test-") + testing::UnitTest::GetInstance()->current_test_info()->name()
            + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
    }
    void text(const fs::path &path, const std::string &value = "") {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary); file << value;
        if (!file) throw std::runtime_error("cannot write external resource fixture");
    }
    void tree(const fs::path &path) {
        fs::create_directories(path / "es"); text(path / "assets/main.mr", "fixture");
    }
    fs::path source, root;
};
class Resources : public ResourceFixture {};
class SceneReader : public ResourceFixture {
protected:
    std::vector<unsigned char> original() {
        std::ifstream file(source / "assets/assets.ysce", std::ios::binary);
        if (!file) throw std::runtime_error("missing supplied scene fixture");
        std::ostringstream data; data << file.rdbuf();
        const auto bytes = data.str(); return {bytes.begin(), bytes.end()};
    }
    fs::path scene(const std::vector<unsigned char> &bytes) {
        const auto path = root / (std::to_string(sequence++) + ".ysce");
        std::ofstream file(path, std::ios::binary);
        if (!bytes.empty()) file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        if (!file) throw std::runtime_error("cannot write external scene fixture");
        return path;
    }
    static void store(std::vector<unsigned char> &bytes, size_t offset, uint32_t value, unsigned length = 4) {
        for (unsigned i = 0; i < length; ++i) bytes[offset + i] = static_cast<unsigned char>((value >> (8 * i)) & 255);
    }
    int sequence = 0;
};

TEST_F(Resources, InstalledAndBundleRootsIgnoreWorkingDirectory) {
    const auto prefix = root / fs::u8path("relocated space \xCE\xA9");
    const auto exe = prefix / "bin/app";
    text(exe); tree(prefix / "share/engine-sim");
    const auto decoy = root / "decoy"; tree(decoy); text(decoy / "delta.conf", "wrong\nwrong\n");
    WorkingDirectory cwd(decoy);
    const auto paths = ResourcePaths::resolve(exe, {}, true);
    EXPECT_EQ(paths.root, fs::canonical(prefix / "share/engine-sim"));
    EXPECT_EQ(paths.script, paths.assets / "main.mr");
    EXPECT_NO_THROW(paths.validateBase());
    const auto bundle = prefix / "EngineSim.app/Contents";
    text(bundle / "MacOS/app"); tree(bundle / "Resources");
    EXPECT_EQ(ResourcePaths::resolve(bundle / "MacOS/app", {}).root, fs::canonical(bundle / "Resources"));
}

TEST_F(Resources, ExplicitRootsAndScriptsOverrideLegacyConfiguration) {
    const auto exe = root / "program/bin/app"; text(exe);
    text(exe.parent_path() / "delta.conf", "wrong\nwrong\n");
    const auto data = root / fs::u8path("data \xC3\xA1"); tree(data);
    const auto custom = root / fs::u8path("custom \xCE\xA9/main.mr"); text(custom);
    const auto args = std::vector<std::string>{"ignored-argv0", "--resources", data.u8string(), "--script", custom.u8string()};
    auto paths = ResourcePaths::resolve(exe, engine_sim::parseResourceOptions(args), true);
    EXPECT_EQ(paths.root, fs::canonical(data)); EXPECT_EQ(paths.script, fs::canonical(custom));
    EXPECT_NO_THROW(paths.validateBase());
    WorkingDirectory cwd(root);
    paths = ResourcePaths::resolve(exe, {fs::u8path("data \xC3\xA1"), fs::u8path("custom \xCE\xA9/main.mr")});
    EXPECT_EQ(paths.root, fs::canonical(data)); EXPECT_EQ(paths.script, fs::canonical(custom));
}

TEST_F(Resources, LegacyConfigurationIsModuleRelativeAndUtf8) {
    const auto exe = root / "bin/app"; text(exe); tree(exe.parent_path());
    text(exe.parent_path() / "delta.conf", "/delta\r\n/assets\r\n");
    auto paths = ResourcePaths::resolve(exe, {}, true);
    EXPECT_EQ(paths.assets, exe.parent_path() / "assets");
    EXPECT_EQ(paths.library, exe.parent_path() / "es");
    EXPECT_EQ(paths.desktop, exe.parent_path() / "delta");
    EXPECT_NO_THROW(paths.validateBase());
    const auto data = root / fs::u8path("local \xCE\xA9"); tree(data);
    text(exe.parent_path() / "delta.conf", "\xEF\xBB\xBF../local \xCE\xA9/delta\r\n../local \xCE\xA9/assets\r\n");
    paths = ResourcePaths::resolve(exe, {}, true);
    EXPECT_EQ(paths.root, fs::canonical(data)); EXPECT_EQ(paths.desktop, data / "delta");
    EXPECT_NO_THROW(paths.validateBase());
    text(exe.parent_path() / "delta.conf", (data / "delta").u8string() + "\n" + (data / "assets").u8string() + "\n");
    EXPECT_EQ(ResourcePaths::resolve(exe, {}, true).assets, data / "assets");
}

TEST_F(Resources, MissingInputsAndMalformedOptionsNameTheFailure) {
    EXPECT_THROW(engine_sim::parseResourceOptions({"app", "--resources"}), ResourceError);
    EXPECT_THROW(engine_sim::parseResourceOptions({"app", "--script"}), ResourceError);
    EXPECT_THROW(engine_sim::parseResourceOptions({"app", "--resources", "a", "--resources", "b"}), ResourceError);
    EXPECT_THROW(engine_sim::parseResourceOptions({"app", "--unknown"}), ResourceError);
    const auto exe = root / "bin/app"; text(exe);
    text(exe.parent_path() / "delta.conf", "one-line\n");
    EXPECT_THROW(ResourcePaths::resolve(exe, {}, true), ResourceError);
    tree(root / "data"); auto paths = ResourcePaths::resolve(exe, {root / "data", {}});
    fs::remove(paths.script);
    try { paths.validateBase(); FAIL() << "accepted missing script"; }
    catch (const ResourceError &error) { EXPECT_NE(std::string(error.what()).find(paths.script.u8string()), std::string::npos); }
    text(paths.script);
    EXPECT_THROW(paths.validateLegacyDesktop(), ResourceError);
}

TEST_F(Resources, NativeExecutableLookupSurvivesDirectoryChanges) {
    const auto executable = engine_sim::executablePath();
    ASSERT_TRUE(fs::is_regular_file(executable)); ASSERT_TRUE(executable.is_absolute());
    const auto directory = root / fs::u8path("different cwd \xCE\xA9"); fs::create_directory(directory);
    WorkingDirectory cwd(directory);
    EXPECT_EQ(engine_sim::executablePath(), executable);
}

TEST_F(SceneReader, LoadsEveryBundledMeshWithOriginalTopology) {
    const auto loaded = engine_sim::readCompiledScene(source / "assets/assets.ysce");
    ASSERT_EQ(loaded.meshes.size(), 9U);
    const std::vector<std::string> names{"Crankshaft", "ConnectingRod", "Piston", "CylinderHead", "Valve",
        "Valve.001", "CrankSnoutThreads", "CrankSnout", "Logo"};
    const std::vector<size_t> vertices{663, 476, 1485, 4195, 1625, 1625, 33, 191, 18};
    const std::vector<size_t> faces{1240, 776, 2736, 7984, 3072, 3072, 32, 252, 18};
    for (size_t i = 0; i < names.size(); ++i) {
        EXPECT_EQ(loaded.meshes[i].name, names[i]); EXPECT_EQ(loaded.meshes[i].vertices.size(), vertices[i]);
        EXPECT_EQ(loaded.meshes[i].indices.size(), faces[i] * 3);
    }
}

TEST_F(SceneReader, RejectsTruncatedAndOversizedContainers) {
    const auto bytes = original();
    for (const size_t size : {size_t{0}, size_t{3}, size_t{4}, size_t{100}, size_t{255}, bytes.size() - 1}) {
        SCOPED_TRACE(size);
        EXPECT_THROW(engine_sim::readCompiledScene(scene({bytes.begin(), bytes.begin() + size})), ResourceError);
    }
    const auto path = scene({});
    { std::ofstream file(path, std::ios::binary); file.seekp(8 * 1024 * 1024); file.put(0); ASSERT_TRUE(file); }
    EXPECT_THROW(engine_sim::readCompiledScene(path), ResourceError);
}

TEST_F(SceneReader, RejectsInvalidLayoutsCoordinatesAndIndices) {
    const auto bytes = original();
    for (const auto &change : {std::pair<size_t, uint32_t>{0, UINT32_MAX}, {4 + 144, 7}, {4 + 224, 0},
            {4 + 228, UINT32_MAX}, {4 + 236, 1}, {4 + 244, 15}, {4 + 248, 0}, {4 + 148, 0x7f800000}, {256, 0x7fc00000}}) {
        SCOPED_TRACE(change.first);
        auto corrupt = bytes; store(corrupt, change.first, change.second);
        EXPECT_THROW(engine_sim::readCompiledScene(scene(corrupt)), ResourceError);
    }
    auto corrupt = bytes; store(corrupt, 256 + 26520, UINT16_MAX, 2);
    EXPECT_THROW(engine_sim::readCompiledScene(scene(corrupt)), ResourceError);
}

TEST_F(SceneReader, RejectsMissingModelsAndParentCycles) {
    const auto bytes = original();
    auto corrupt = bytes; std::fill(corrupt.begin() + 4, corrupt.begin() + 68, 'A');
    EXPECT_THROW(engine_sim::readCompiledScene(scene(corrupt)), ResourceError);
    corrupt = bytes; corrupt[4] = 'X';
    EXPECT_THROW(engine_sim::readCompiledScene(scene(corrupt)), ResourceError);
    corrupt = bytes; store(corrupt, 4 + 132, 0);
    EXPECT_THROW(engine_sim::readCompiledScene(scene(corrupt)), ResourceError);
    corrupt = bytes; store(corrupt, 4 + 132, 99);
    EXPECT_THROW(engine_sim::readCompiledScene(scene(corrupt)), ResourceError);
}
}
