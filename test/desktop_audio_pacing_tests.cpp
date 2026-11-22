#include "desktop_audio_pacer.h"
#include "../src/runtime/desktop_runtime.hpp"
#include "prepared_engine.h"
#include "pull_engine.h"
#include "simulation_fault.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <chrono>
#include <fstream>
#include <sstream>
#include <limits>

namespace engine_sim {
struct RuntimeTestAccess {
    static void corruptDesktopBody(desktop::RuntimeHost &host) {
        host.m_runtime.m_session->prepared.session->definition.engine->getCrankshaft(0)->m_body.p_x =
            std::numeric_limits<double>::quiet_NaN();
    }
};
}
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

namespace fs = std::filesystem;
fs::path runtimeFixture() {
    const auto *root=std::getenv("ENGINE_SIM_TEST_SOURCE");
    if (!root) throw std::runtime_error("Missing explicit fixture source");
    return fs::u8path(root)/"examples/runtime/fixture";
}
es_script::PreparationContext recipeAt(const fs::path &root) { return {root/"main.mr",root/"library",root}; }
struct DesktopFiles {
    fs::path root=fs::current_path()/fs::u8path("owner space Ω-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    DesktopFiles() { fs::copy(runtimeFixture(),root,fs::copy_options::recursive); }
    ~DesktopFiles() { std::error_code error;fs::remove_all(root,error); }
};
float runtimeSample(desktop::RuntimeHost &host) {
    float sample=123;std::uint32_t written=0;host.render(&sample,1,written);
    if (written!=1) throw engine_sim::SimulationFault("Missing source frame");
    return sample;
}

TEST(RuntimeDesktop, StoppedPartialAndWrappedDeviceRequestsUseOnlyCommittedRuntimePcm) {
    auto host=desktop::RuntimeHost::prepare(recipeAt(runtimeFixture()));
    host->commands().setStarter(true);host->commands().setIgnition(true);host->commands().setDemand(.25);host->commands().flush();
    std::vector<std::int16_t> expected;
    AudioPacer pacer([&] { const auto sample=runtimeSample(*host);expected.push_back(static_cast<std::int16_t>(sample*32768));return sample; });
    Device device;device.partial=13;
    const auto service=[&] {
        host->beginPresentation();const bool result=pacer.service(device,1,false);host->endPresentation();return result;
    };
    ASSERT_TRUE(service());const auto filled=host->counts().frames;EXPECT_EQ(filled,AudioPacer::Target);
    for (int i=0;i<100;++i) { ASSERT_TRUE(service());EXPECT_EQ(host->counts().frames,filled);EXPECT_EQ(host->presentationTicks(),0u);EXPECT_DOUBLE_EQ(host->renderCpuSeconds(),0); }
    for (int i=0;i<200;++i) {device.consume(10);ASSERT_TRUE(service());}
    EXPECT_EQ(host->counts().frames,pacer.statistics().generatedSource);
    EXPECT_EQ(pacer.statistics().requestedSource,pacer.statistics().generatedSource);
    EXPECT_EQ(host->counts().frames,filled+2000);EXPECT_EQ(host->telemetry().frames,host->counts().frames);
    ASSERT_LE(device.submitted.size(),expected.size());EXPECT_TRUE(std::equal(device.submitted.begin(),device.submitted.end(),expected.begin()));
    EXPECT_EQ(device.silence,0u);EXPECT_EQ(pacer.statistics().discarded,0u);EXPECT_EQ(pacer.statistics().sourceFaults,0u);
    EXPECT_LE(pacer.statistics().maximumQueued,AudioPacer::Target);
    EXPECT_EQ(host->simulator()->synthesizer().getDroppedInputSamples(),0u);
    EXPECT_EQ(host->simulator()->synthesizer().getDroppedOutputSamples(),0u);
}

TEST(RuntimeDesktop, PartitionsRateChangesAndNamedCommandsPreservePcmAndPhysicalPhase) {
    auto a=desktop::RuntimeHost::prepare(recipeAt(runtimeFixture())),b=desktop::RuntimeHost::prepare(recipeAt(runtimeFixture()));
    const std::array<std::uint32_t,9> events{0,1,147,160,1600,4409,6000,7000,10000};
    const auto render=[&](desktop::RuntimeHost &host,bool chopped) {
        std::vector<float> output(12000);std::size_t first=0;
        for (std::size_t event=0;event<events.size();++event) {
            const auto at=events[event],end=event+1<events.size()?events[event+1]:static_cast<std::uint32_t>(output.size());
            auto &commands=host.commands();commands.setDemand(event%2?.6:.25);commands.setIgnition(event!=8);commands.setStarter(event<4);
            commands.setClutch(event>=6?.5:0);commands.changeGear(event>=6?0:-1);
            commands.setDyno(event==7,true,units::rpm(1000));
            commands.setFrequency(event==5?4000:event==6?32000:10000);
            auto mixer=commands.mixer();mixer.volume=event%2?.4f:.25f;commands.setMixer(mixer);commands.flush();
            host.requestFrequency(commands.pending.frequency);
            EXPECT_EQ(first,at);
            for (;first<end;) {
                const auto count=static_cast<std::uint32_t>(chopped?std::min<std::size_t>(1+first%97,end-first):end-first);
                std::uint32_t written=0;host.beginPresentation();host.render(output.data()+first,count,written);host.endPresentation();
                EXPECT_EQ(written,count);first+=written;
            }
        }
        return output;
    };
    const auto first=render(*a,false),second=render(*b,true);
    EXPECT_EQ(first,second);EXPECT_EQ(a->counts().frames,12000u);EXPECT_EQ(a->counts().nativeFrames,12000u);
    EXPECT_EQ(a->counts().ticks,b->counts().ticks);
    EXPECT_EQ(a->simulator()->getEngine()->getCrankshaft(0)->m_body.theta,b->simulator()->getEngine()->getCrankshaft(0)->m_body.theta);
    EXPECT_EQ(a->simulator()->getTransmission()->getGear(),0);
    EXPECT_DOUBLE_EQ(a->simulator()->getTransmission()->getClutchPressure(),.5);
}

TEST(RuntimeDesktop, ResetCandidateVerifiesOwnedInputsAndRetiresPreviousGenerationOnPublication) {
    DesktopFiles files;const auto recipe=recipeAt(files.root);
    auto host=desktop::RuntimeHost::prepare(recipe);
    host->commands().setStarter(true);host->commands().setIgnition(true);host->commands().flush();
    std::array<float,441> samples{};std::uint32_t written=0;host->render(samples.data(),samples.size(),written);
    auto previous=host->views().at<Engine>();auto replacement=host->resetCandidate();
    EXPECT_EQ(host->counts().frames,441u);EXPECT_EQ(replacement->counts().frames,0u);
    EXPECT_NE(previous.generation(),replacement->views().at<Engine>().generation());
    {auto phase=host->views().present();EXPECT_TRUE(previous->getIgnitionModule()->m_enabled);}
    host=std::move(replacement);EXPECT_FALSE(previous.valid());
    {auto phase=host->views().present();EXPECT_THROW(previous->getRpm(),std::logic_error);}
    auto reference=desktop::RuntimeHost::prepare(recipe);
    written=0;host->render(samples.data(),samples.size(),written);
    std::array<float,441> expected{};written=0;reference->render(expected.data(),expected.size(),written);EXPECT_EQ(samples,expected);
    const auto frames=host->counts().frames;auto current=host->views().at<Engine>();
    std::ifstream source(files.root/"impulse.wav",std::ios::binary);ASSERT_TRUE(source.good());
    std::ostringstream savedBytes;savedBytes<<source.rdbuf();const auto original=savedBytes.str();source.close();
    {std::ofstream changed(files.root/"impulse.wav",std::ios::binary);changed<<"changed malformed impulse";}
    try {auto rejected=host->resetCandidate();FAIL()<<"Changed input was accepted";}
    catch(const EngineLoadError &error) {EXPECT_EQ(error.diagnostic.stage,"resources");}
    EXPECT_EQ(host->counts().frames,frames);EXPECT_TRUE(current.valid());
    written=0;host->render(samples.data(),samples.size(),written);written=0;reference->render(expected.data(),expected.size(),written);EXPECT_EQ(samples,expected);
    {std::ofstream restored(files.root/"impulse.wav",std::ios::binary);restored<<original;}
}

TEST(RuntimeDesktop, PrivatePreparationAdmitsLegacyRatesWithoutChangingPublicLimitsOrSeed) {
    DesktopFiles files;const auto recipe=recipeAt(files.root);
    std::ifstream source(files.root/"main.mr");std::ostringstream contents;contents<<source.rdbuf();source.close();const auto original=contents.str();
    const auto field=original.find("simulation_frequency: 10000");ASSERT_NE(field,std::string::npos);
    for (double declared:{400.0,400000.0,400.75,400000.75,10000.75}) {
        const auto frequency=static_cast<unsigned>(declared);
        auto text=original;text.replace(field,std::string("simulation_frequency: 10000").size(),"simulation_frequency: "+std::to_string(declared));
        {std::ofstream target(files.root/"main.mr");target<<text;}
        EXPECT_THROW(engine_sim::prepareEngine(recipe),EngineLoadError);
        auto host=desktop::RuntimeHost::prepare(recipe);EXPECT_EQ(host->simulator()->getSimulationFrequency(),frequency);
        auto view=host->views().at<Engine>();const auto generation=view.generation();
        float sample;std::uint32_t written=0;host->render(&sample,1,written);EXPECT_EQ(written,1u);
        if(frequency==400) {
            EXPECT_EQ(host->counts().ticks,0u);
            host->commands().setFrequency(400000);host->commands().flush();host->requestFrequency(400000);
            std::array<float,111> rest{};written=0;host->render(rest.data(),rest.size(),written);
            EXPECT_EQ(host->counts().frames,112u);EXPECT_EQ(host->counts().ticks,16u);
        }
        EXPECT_EQ(view.generation(),generation);
    }
    std::vector<es_script::ScriptInput> inputs;auto definition=es_script::Compiler::prepare(recipe,inputs);
    definition.engine->setRandomSeed(123);auto adopted=desktop::RuntimeHost::adopt(std::move(definition));
    EXPECT_EQ(adopted->simulator()->getEngine()->getRandomSeed(),123u);
}

TEST(RuntimeDesktop, FaultsPublishOnlyCompletedFramesAndNeverAdvanceWhileAViewIsLeased) {
    static_assert(desktop::RuntimeHost::MaxRenderFrames==AudioPacer::Capacity);
    auto host=desktop::RuntimeHost::prepare(recipeAt(runtimeFixture()));
    std::uint32_t empty=0;host->render(nullptr,0,empty);EXPECT_EQ(host->counts().frames,0u);
    float canary=123;EXPECT_THROW(host->render(&canary,desktop::RuntimeHost::MaxRenderFrames+1,empty),std::length_error);
    EXPECT_FLOAT_EQ(canary,123);EXPECT_EQ(host->counts().frames,0u);EXPECT_EQ(host->telemetry().state,ES_READY);
    std::array<float,441> output;output.fill(123);std::uint32_t written=0;
    {auto phase=host->views().present();EXPECT_THROW(host->render(output.data(),output.size(),written),std::logic_error);
        EXPECT_THROW(host->endPresentation(),std::logic_error); }
    EXPECT_EQ(written,0u);EXPECT_EQ(host->counts().frames,0u);
    engine_sim::RuntimeTestAccess::corruptDesktopBody(*host);
    EXPECT_THROW(host->render(output.data(),output.size(),written),engine_sim::SimulationFault);
    EXPECT_EQ(written,4u);EXPECT_EQ(host->telemetry().frames,written);EXPECT_EQ(host->telemetry().state,ES_FAULTED);
    EXPECT_TRUE(std::all_of(output.begin()+written,output.end(),[](float sample){return sample==123;}));
    const auto frames=host->counts().frames;written=0;EXPECT_THROW(host->render(output.data(),1,written),engine_sim::SimulationFault);
    EXPECT_EQ(written,0u);EXPECT_EQ(host->counts().frames,frames);
    auto transported=desktop::RuntimeHost::prepare(recipeAt(runtimeFixture()));engine_sim::RuntimeTestAccess::corruptDesktopBody(*transported);
    Device device;AudioPacer pacer([&] {return runtimeSample(*transported);});
    EXPECT_FALSE(pacer.service(device,1,false));EXPECT_EQ(pacer.statistics().sourceFaults,1u);
    EXPECT_EQ(pacer.statistics().generatedSource,4u);EXPECT_EQ(device.submitted.size(),4u);
    EXPECT_EQ(transported->telemetry().frames,4u);EXPECT_EQ(pacer.statistics().discarded,0u);
}

}
