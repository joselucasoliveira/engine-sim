#include <gtest/gtest.h>
#include "desktop_run.h"
#include "virtual_audio.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {
TEST(DesktopRun, NormalArgumentsRetainExplicitResourceAndAudioSelection) {
    const auto options=desktop::parseRunOptions({"app","--resources","resources","--script","entry.mr","--audio-mode","virtual"});
    EXPECT_FALSE(options.smoke); EXPECT_TRUE(options.virtualAudio);
    EXPECT_EQ(options.resources.resources,"resources"); EXPECT_EQ(options.resources.script,"entry.mr");
    EXPECT_TRUE(desktop::parseRunOptions({"app","--help"}).help);
}
TEST(DesktopRun, RejectsUnknownDuplicateUnboundedAndIncompleteArguments) {
    for (const auto &args : std::vector<std::vector<std::string>>{
        {"app","--unknown"},{"app","--smoke","--smoke"},{"app","--frames","0"},
        {"app","--frames","10001"},{"app","--frames","2x"},{"app","--frames","60"},
        {"app","--report"},{"app","--audio-mode","silent"},{"app","--script","a","--script","b"}})
        EXPECT_THROW(desktop::parseRunOptions(args),desktop::DesktopError);
}
TEST(DesktopRun, ReportsRequireFreshAbsolutePathsOutsideResources) {
    namespace fs=std::filesystem;
    const auto directory=fs::current_path()/("run-options-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(directory); const auto report=directory/"new report.json";
    const auto options=desktop::parseRunOptions({"app","--smoke","--frames","60","--report",report.u8string()});
    EXPECT_EQ(options.frames,60U); EXPECT_EQ(options.report,report);
    EXPECT_THROW(desktop::parseRunOptions({"app","--smoke","--report","relative.json"}),desktop::DesktopError);
    EXPECT_THROW(desktop::parseRunOptions({"app","--smoke","--report",report.u8string(),"--resources",directory.u8string()}),desktop::DesktopError);
    { std::ofstream existing(report); existing << "owned bytes"; }
    EXPECT_THROW(desktop::parseRunOptions({"app","--smoke","--report",report.u8string()}),desktop::DesktopError);
    fs::remove(report); fs::remove(directory);
}
TEST(DesktopRun, MissingStagesCannotProduceSuccessfulEvidence) {
    desktop::RunEvidence evidence; evidence.options.smoke=true;
    evidence.frames=evidence.options.frames; evidence.tornDown=true;
    EXPECT_THROW(evidence.validate(),desktop::DesktopError);
}
TEST(DesktopRun, LegacyVirtualSinkConsumesActualSamplesWithoutADevice) {
    desktop::VirtualAudio audio; audio.open();
    auto span=audio.lock(0,1000); std::fill_n(span.first,span.firstCount,1234);
    audio.unlock(span); audio.setPlaying(true); std::this_thread::sleep_for(std::chrono::milliseconds(35));
    EXPECT_GT(audio.cursor(),1000U); EXPECT_EQ(audio.source,1000U); EXPECT_EQ(audio.nonzero,1000U);
    EXPECT_EQ(audio.consumed,audio.source+audio.silence);
    audio.quiesce(); const auto cursor=audio.cursor(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); EXPECT_EQ(audio.cursor(),cursor);
    audio.shutdown(); audio.shutdown(); EXPECT_THROW(audio.cursor(),desktop::DesktopError);
}
}
