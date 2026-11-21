#include <gtest/gtest.h>
#include "sdl_audio.h"
#include "engine_sim_application.h"
#include "recording_host.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {
using Audio = desktop::SdlAudioOutput;
using namespace std::chrono_literals;
struct Observation {
    std::array<std::int16_t, 200000> samples{};
    std::atomic<std::size_t> count{0};
    static void receive(void *context, const std::int16_t *samples, std::size_t count) noexcept {
        auto &o = *static_cast<Observation *>(context);
        const auto offset = o.count.load(std::memory_order_relaxed);
        if (offset + count <= o.samples.size()) std::copy_n(samples, count, o.samples.data() + offset);
        o.count.store(offset + count, std::memory_order_release);
    }
};
template<class Predicate> bool eventually(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + 8s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}
void submit(Audio &audio, const std::int16_t *samples, unsigned count, unsigned offset = 43900) {
    auto span = audio.lock(offset, count);
    std::copy_n(samples, span.firstCount, span.first);
    std::copy_n(samples + span.firstCount, span.secondCount, span.second);
    audio.unlock(span);
}
Audio::Options options(Observation *o = nullptr) {
    Audio::Options value; value.mode = Audio::Mode::Virtual;
    if (o) { value.observeSource = Observation::receive; value.observer = o; }
    return value;
}
TEST(DesktopAudio, OrderedSourceWrapBoundedQueueAndSeparateSilence) {
    Observation observed; Audio audio; audio.open(options(&observed));
    const auto device = audio.deviceInfo();
    EXPECT_EQ(device.driver, "dummy"); EXPECT_TRUE(device.virtualDevice);
    EXPECT_EQ(device.sourceFrequency, 44100); EXPECT_EQ(device.sourceChannels, 1);
    EXPECT_EQ(device.sourceFormat, SDL_AUDIO_S16); EXPECT_GT(device.deviceFrequency, 0);
    EXPECT_GT(device.deviceChannels, 0); EXPECT_GT(device.deviceQuantum, 0);
    std::ofstream report("virtual-device.txt");
    report << "Explicit virtual endpoint; no physical loopback claim\ndriver=" << device.driver << "\nname=" << device.name
           << "\nsource=" << device.sourceFrequency << ',' << device.sourceChannels << ',' << device.sourceFormat
           << "\ndevice=" << device.deviceFrequency << ',' << device.deviceChannels << ',' << device.deviceFormat
           << "\nquantum=" << device.deviceQuantum << '\n';
    std::array<std::int16_t, 100003> expected{};
    for (std::size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<std::int16_t>((i * 37) % 60001 - 30000);
    submit(audio, expected.data(), 3000); audio.setPlaying(true);
    unsigned written = 3000;
    while (written < expected.size()) {
        ASSERT_TRUE(eventually([&] { return audio.statistics().submittedSource - audio.statistics().pumpedSource < 20000; }));
        const auto count = std::min<unsigned>(997, expected.size() - written);
        submit(audio, expected.data() + written, count); written += count;
    }
    ASSERT_TRUE(eventually([&] { audio.check(); return observed.count.load(std::memory_order_acquire) == expected.size(); }));
    ASSERT_TRUE(eventually([&] { return audio.statistics().starvationSilence > 0; }));
    audio.quiesce(); EXPECT_FALSE(audio.running());
    const auto stats = audio.statistics();
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), observed.samples.begin()));
    EXPECT_EQ(stats.submittedSource, expected.size()); EXPECT_EQ(stats.pumpedSource, expected.size());
    EXPECT_EQ(stats.discardedSource, 0); EXPECT_EQ(stats.primingSilence, Audio::Watermark);
    EXPECT_EQ(stats.putFrames, stats.pumpedSource + stats.primingSilence + stats.starvationSilence);
    EXPECT_EQ(stats.attemptedFrames, stats.putFrames); EXPECT_GT(stats.starvationBlocks, 0);
    EXPECT_LE(stats.maxQueuedFrames, Audio::Watermark); EXPECT_LE(stats.maxSourceFrames, Audio::Capacity);
    EXPECT_EQ(stats.overflows, 0); EXPECT_GT(stats.consumedInput, 0);
    EXPECT_EQ(audio.cursor(), stats.consumedInput % Audio::Capacity);
    EXPECT_LT(stats.joinNanoseconds, 3000000000ULL);
    report << "\nsubmitted-source=" << stats.submittedSource << "\npumped-source=" << stats.pumpedSource
           << "\npriming-silence=" << stats.primingSilence << "\nstarvation-silence=" << stats.starvationSilence
           << "\nSDL-input-consumed=" << stats.consumedInput << "\nmax-queued-frames=" << stats.maxQueuedFrames
           << "\njoin-nanoseconds=" << stats.joinNanoseconds << '\n';
}
TEST(DesktopAudio, SourceCapacityInvalidSpansAndRepeatedLifetime) {
    Audio audio;
    for (int iteration = 0; iteration < 6; ++iteration) {
        audio.open(options()); EXPECT_THROW(audio.open(options()), desktop::DesktopError);
        EXPECT_THROW(audio.lock(44100, 1), desktop::DesktopError);
        auto span = audio.lock(44099, Audio::Capacity);
        EXPECT_EQ(span.firstCount, 1U); EXPECT_EQ(span.secondCount, 44099U);
        EXPECT_THROW(audio.lock(0, 1), desktop::DesktopError);
        auto wrong = span; ++wrong.firstCount; EXPECT_THROW(audio.unlock(wrong), desktop::DesktopError);
        audio.unlock(span); EXPECT_THROW(audio.unlock(span), desktop::DesktopError);
        auto full = audio.lock(0, 1); EXPECT_THROW(audio.unlock(full), desktop::DesktopError);
        EXPECT_EQ(audio.statistics().overflows, 1U);
        for (int start = 0; start < 3; ++start) {
            audio.setPlaying(true); std::this_thread::sleep_for(5ms);
            audio.setPlaying(false); EXPECT_FALSE(audio.running());
        }
        audio.shutdown(); audio.shutdown(); EXPECT_EQ(SDL_WasInit(SDL_INIT_AUDIO), 0U);
    }
}
bool failResume(SDL_AudioStream *) { return SDL_SetError("injected device resume failure"); }
std::atomic<int> putCalls{0};
bool failFeed(SDL_AudioStream *stream, const void *data, int bytes) {
    if (putCalls.fetch_add(1) > 0) return SDL_SetError("injected endpoint write failure");
    return SDL_PutAudioStreamData(stream, data, bytes);
}
int failQueue(SDL_AudioStream *) { SDL_SetError("injected endpoint queue failure"); return -1; }
int stalledQueue(SDL_AudioStream *) { return Audio::Watermark * 2; }
TEST(DesktopAudio, StalledDeviceKeepsBothQueuesBoundedAndStopsPromptly) {
    Audio audio; auto value = options(); value.queued = stalledQueue; audio.open(value);
    std::array<std::int16_t, Audio::Capacity> source{};
    submit(audio, source.data(), source.size()); audio.setPlaying(true);
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(audio.statistics().pumpedSource, 0U); EXPECT_EQ(audio.statistics().putFrames, Audio::Watermark);
    auto span = audio.lock(0, 1); EXPECT_THROW(audio.unlock(span), desktop::DesktopError);
    std::thread foreign([&] { EXPECT_THROW(audio.lock(0, 1), desktop::DesktopError); }); foreign.join();
    audio.quiesce(); EXPECT_FALSE(audio.running()); EXPECT_LT(audio.statistics().joinNanoseconds, 3000000000ULL);
}
TEST(DesktopAudio, FailedOpenResumeAndFeedRollbackWithoutDeadlock) {
    Audio audio;
    ASSERT_TRUE(SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "engine-sim-unavailable", SDL_HINT_OVERRIDE));
    EXPECT_THROW(audio.open(options()), desktop::DesktopError); EXPECT_EQ(SDL_WasInit(SDL_INIT_AUDIO), 0U);
    SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
    auto value = options(); value.mode = Audio::Mode::Device;
    EXPECT_THROW(audio.open(value), desktop::DesktopError); EXPECT_EQ(SDL_WasInit(SDL_INIT_AUDIO), 0U);
    value = options(); value.resume = failResume; audio.open(value);
    EXPECT_THROW(audio.setPlaying(true), desktop::DesktopError); EXPECT_FALSE(audio.running());
    audio.shutdown(); EXPECT_EQ(SDL_WasInit(SDL_INIT_AUDIO), 0U);
    value = options(); value.put = failFeed; putCalls.store(0); audio.open(value); audio.setPlaying(true);
    ASSERT_TRUE(eventually([&] { return !audio.running(); })); EXPECT_THROW(audio.check(), desktop::DesktopError);
    audio.shutdown(); EXPECT_EQ(SDL_WasInit(SDL_INIT_AUDIO), 0U);
    value = options(); value.queued = failQueue; audio.open(value); audio.setPlaying(true);
    ASSERT_TRUE(eventually([&] { return !audio.running(); })); EXPECT_THROW(audio.cursor(), desktop::DesktopError);
    audio.shutdown(); EXPECT_EQ(SDL_WasInit(SDL_INIT_AUDIO), 0U);
    audio.open(options()); audio.setPlaying(true); audio.shutdown(); EXPECT_FALSE(audio.running());
}
TEST(DesktopAudio, DeviceRemovalStopsOnlyTheBoundEndpoint) {
    Audio audio; audio.open(options()); audio.setPlaying(true);
    SDL_Event event{}; event.type = SDL_EVENT_AUDIO_DEVICE_REMOVED; event.adevice.which = 123456;
    ASSERT_TRUE(SDL_PushEvent(&event)); EXPECT_NO_THROW(audio.check());
    event.adevice.which = audio.deviceInfo().logicalDevice;
    ASSERT_TRUE(SDL_PushEvent(&event)); EXPECT_THROW(audio.check(), desktop::DesktopError);
    EXPECT_FALSE(audio.running()); audio.shutdown(); EXPECT_EQ(SDL_WasInit(SDL_INIT_AUDIO), 0U);
}
TEST(DesktopAudio, SdlConvertsOnlyAtTheDeviceBoundary) {
    ASSERT_TRUE(SDL_InitSubSystem(SDL_INIT_AUDIO));
    SDL_AudioSpec input{SDL_AUDIO_S16, 1, 44100}, output{SDL_AUDIO_F32, 2, 48000};
    auto *stream = SDL_CreateAudioStream(&input, &output); ASSERT_NE(stream, nullptr);
    std::array<std::int16_t, 4410> source{}; source.fill(8192);
    ASSERT_TRUE(SDL_PutAudioStreamData(stream, source.data(), source.size() * 2));
    ASSERT_TRUE(SDL_FlushAudioStream(stream));
    const int bytes = SDL_GetAudioStreamAvailable(stream); ASSERT_EQ(bytes, 4800 * 2 * 4);
    std::vector<float> converted(bytes / 4); ASSERT_EQ(SDL_GetAudioStreamData(stream, converted.data(), bytes), bytes);
    for (std::size_t frame = 100; frame < 4700; ++frame) {
        EXPECT_FLOAT_EQ(converted[frame * 2], converted[frame * 2 + 1]);
        EXPECT_NEAR(converted[frame * 2], .25f, .0001f);
    }
    SDL_DestroyAudioStream(stream); SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
class AudioHost final : public desktop::Host {
public:
    explicit AudioHost(std::shared_ptr<host_test::Recording> record, Observation &observed)
        : fixture(std::move(record)), observed(observed) { fixture.inputs.resize(45); for (auto &input : fixture.inputs) input.hold(desktop::Key::S); fixture.inputs[0].press(desktop::Key::A); }
    host_test::RecordingHost fixture;
    Audio audio;
    Observation &observed;
    std::vector<std::int16_t> expected;
    void initialize(const engine_sim::ResourcePaths &paths, const engine_sim::CompiledScene &scene) override { fixture.initialize(paths, scene); loadModels(scene); }
    void shutdown() noexcept override { audio.shutdown(); fixture.shutdown(); }
    void StartFrame() override { clearPackets(); fixture.StartFrame(); audio.check(); }
    void EndFrame() override { std::this_thread::sleep_for(16ms); }
    const desktop::FrameState &frame() const override { return fixture.frame(); }
    bool IsKeyDown(desktop::Key key) override { return fixture.IsKeyDown(key); }
    bool ProcessKeyDown(desktop::Key key) override { return fixture.ProcessKeyDown(key); }
    bool ProcessMouseButtonDown(desktop::Button b) override { return fixture.ProcessMouseButtonDown(b); }
    bool ProcessMouseButtonUp(desktop::Button b) override { return fixture.ProcessMouseButtonUp(b); }
    int GetMouseWheel() override { return fixture.GetMouseWheel(); }
    void GetOsMousePos(int *x, int *y) override { fixture.GetOsMousePos(x,y); }
    void setFullscreen(bool value) override { fixture.setFullscreen(value); }
    void uploadGeometry(const desktop::Vertex *v, std::size_t n, const std::uint16_t *i, std::size_t m) override { validateGeometry(v,n,i,m); }
    float measureText(const std::string &text, float height) const override { return fixture.measureText(text,height); }
    void openAudio() override { audio.open(options(&observed)); }
    void setAudioPlaying(bool value) override { audio.setPlaying(value); }
    void quiesceAudio() noexcept override { audio.quiesce(); }
    desktop::AudioOffset audioCursor() override { return audio.cursor(); }
    desktop::AudioSegments lockAudio(desktop::AudioOffset offset, desktop::AudioOffset count) override { return audio.lock(offset,count); }
    void unlockAudio(const desktop::AudioSegments &span) override {
        expected.insert(expected.end(),span.first,span.first+span.firstCount);
        expected.insert(expected.end(),span.second,span.second+span.secondCount);
        audio.unlock(span);
    }
    void readScreen(std::uint8_t *, std::size_t) override { throw desktop::DesktopError("readback","audio fixture has no GPU"); }
};
TEST(DesktopAudio, ActualNineCylinderSourcePreservesOrderAcrossRejectedAndAcceptedReload) {
    const char *root = std::getenv("ENGINE_SIM_TEST_RESOURCES"); ASSERT_NE(root,nullptr);
    const auto resources = std::filesystem::canonical(root);
    const auto script = std::filesystem::absolute("audio entry.mr");
    const std::string valid = "import \"engine_sim.mr\"\nimport \"engines/atg-video-2/09_radial_9.mr\"\nmain()\n";
    { std::ofstream out(script); out << valid; }
    Observation observed; auto recording = std::make_shared<host_test::Recording>();
    auto backend = std::make_unique<AudioHost>(recording, observed); auto *host = backend.get();
    EngineSimApplication app; app.initialize(std::move(backend), {resources, script});
    auto *original = app.getSimulator(); ASSERT_NE(original,nullptr); ASSERT_EQ(original->getEngine()->getCylinderCount(),9);
    EXPECT_FALSE(app.loadEngine(nullptr,nullptr,nullptr)); EXPECT_EQ(app.getSimulator(),original); EXPECT_TRUE(host->audio.running());
    host->fixture.inputs[10].press(desktop::Key::Return);
    host->fixture.inputs[20].press(desktop::Key::Return);
    host->fixture.onFrame = [&](std::size_t frame) {
        if (frame == 10) { std::ofstream out(script); out << "import \"engine_sim.mr\"\nmissing_audio_fixture_node()\n"; }
        if (frame == 11) { EXPECT_EQ(app.getSimulator(),original); EXPECT_TRUE(host->audio.running()); }
        if (frame == 20) { std::ofstream out(script); out << valid; }
        if (frame == 21) { EXPECT_NE(app.getSimulator(),original); EXPECT_TRUE(host->audio.running()); }
    };
    app.run();
    ASSERT_FALSE(host->expected.empty());
    ASSERT_TRUE(std::any_of(host->expected.begin(),host->expected.end(),[](auto sample){ return sample != 0; }));
    host->audio.setPlaying(true);
    ASSERT_TRUE(eventually([&] { host->audio.check(); return observed.count.load(std::memory_order_acquire) == host->expected.size(); }));
    host->audio.quiesce(); EXPECT_EQ(host->audio.statistics().submittedSource, host->expected.size());
    EXPECT_TRUE(std::equal(host->expected.begin(),host->expected.end(),observed.samples.begin()));
    recording->onShutdown = [&] { EXPECT_EQ(app.getSimulator(),nullptr); EXPECT_FALSE(host->audio.running()); };
    app.destroy(); EXPECT_EQ(SDL_WasInit(SDL_INIT_AUDIO),0U); app.destroy();
}
}
int main(int argc,char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "--probe-device") == 0) {
        try {
            Audio audio; audio.open({}); audio.setPlaying(true); std::this_thread::sleep_for(150ms);
            audio.check(); audio.quiesce(); const auto d = audio.deviceInfo(); const auto s = audio.statistics();
            std::printf("driver=%s\nname=%s\ndevice=%d,%d,%u\nquantum=%d\nSDL-input-consumed=%llu\nPhysical playback requires independent loopback evidence.\n",
                d.driver.c_str(),d.name.c_str(),d.deviceFrequency,d.deviceChannels,d.deviceFormat,d.deviceQuantum,
                static_cast<unsigned long long>(s.consumedInput));
            return s.consumedInput > 0 ? 0 : 1;
        } catch (const std::exception &error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
    }
    testing::InitGoogleTest(&argc,argv);
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "SDL_AUDIO_DRIVER", "dummy", true);
    const int result = RUN_ALL_TESTS(); SDL_Quit(); return result;
}
