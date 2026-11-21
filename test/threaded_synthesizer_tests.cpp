#include <gtest/gtest.h>
#include "../include/synthesizer.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <thread>
#include <vector>

namespace {
Synthesizer::Parameters parameters(int outputCapacity = 64) {
    Synthesizer::Parameters p;
    p.inputBufferSize = 512;
    p.audioBufferSize = outputCapacity;
    p.inputSampleRate = p.audioSampleRate = 44100;
    p.initialAudioParameters.airNoise = p.initialAudioParameters.inputSampleNoise = 0;
    p.initialAudioParameters.dF_F_mix = 0;
    p.initialAudioParameters.levelerMaxGain = p.initialAudioParameters.levelerMinGain = 1;
    return p;
}
void setup(Synthesizer &synth, int outputCapacity = 64) {
    synth.initialize(parameters(outputCapacity));
    const int16_t unit[] = {INT16_MAX};
    synth.initializeImpulseResponse(unit, 1, 1, 0);
}
void supply(Synthesizer &synth, int frames, int base = 0) {
    for (int i = 0; i < frames; ++i) {
        const double value = 2048 * std::sin((base + i) * 0.1);
        synth.writeInput(&value);
    }
    synth.endInputBlock();
}
template <typename Predicate> bool until(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < end) {
        if (predicate()) return true;
        std::this_thread::yield();
    }
    return predicate();
}
std::vector<int16_t> renderSequence(bool threaded) {
    Synthesizer synth;
    setup(synth);
    if (threaded) synth.startAudioRenderingThread();
    std::vector<int16_t> result;
    result.reserve(31001);
    for (int block = 0; block < 1000; ++block) {
        auto audio = synth.getAudioParameters();
        audio.volume = block % 2 == 0 ? 1 : 0.5f;
        synth.setAudioParameters(audio);
        supply(synth, 31, block * 31);
        if (threaded) synth.waitProcessed();
        else synth.renderAudio();
        const int expected = block == 0 ? 32 : 31;
        std::array<int16_t, 34> guarded;
        guarded.fill(-999);
        EXPECT_EQ(synth.readAudioOutput(expected, guarded.data() + 1), expected);
        EXPECT_EQ(guarded.front(), -999);
        EXPECT_EQ(guarded[expected + 1], -999);
        result.insert(result.end(), guarded.begin() + 1, guarded.begin() + 1 + expected);
    }
    synth.endAudioRenderingThread();
    const auto stats = synth.getBufferStatistics();
    EXPECT_EQ(stats.rendered, 31001U);
    EXPECT_EQ(stats.delivered, 31001U);
    EXPECT_EQ(stats.droppedInput, 0U);
    EXPECT_EQ(stats.droppedOutput, 0U);
    EXPECT_EQ(stats.zeroFilledOutput, 0U);
    EXPECT_EQ(stats.completedGeneration, 1000U);
    EXPECT_EQ(stats.submittedGeneration, 1000U);
    EXPECT_EQ(synth.getLatency(), 0);
    return result;
}
}

TEST(ThreadedSynthesis, ThousandBlocksMatchSynchronousSequence) {
    const auto serial = renderSequence(false);
    const auto threaded = renderSequence(true);
    EXPECT_EQ(threaded.size(), 31001U);
    EXPECT_EQ(threaded, serial);
}

TEST(ThreadedSynthesis, ProducerControlsAndConsumerOverlap) {
    Synthesizer synth;
    setup(synth, 64);
    synth.startAudioRenderingThread();
    std::atomic<bool> done{false};
    std::atomic<size_t> delivered{0};
    std::promise<void> start;
    auto started = start.get_future().share();
    std::thread controls([&] {
        started.wait();
        int iteration = 0;
        while (!done) {
            auto audio = synth.getAudioParameters();
            audio.volume = (++iteration % 2) == 0 ? 1 : 0.5f;
            synth.setAudioParameters(audio);
            synth.setInputSampleRate(44100);
            EXPECT_TRUE(std::isfinite(synth.getLevelerGain()));
            EXPECT_TRUE(std::isfinite(synth.getLatency()));
        }
    });
    std::thread consumer([&] {
        started.wait();
        while (!done || synth.getBufferStatistics().queuedOutput != 0) {
            const size_t available = synth.getBufferStatistics().queuedOutput;
            if (available == 0) { std::this_thread::yield(); continue; }
            std::array<int16_t, 66> guarded;
            guarded.fill(-999);
            const int count = synth.readAudioOutput(static_cast<int>(available), guarded.data() + 1);
            delivered += count;
            EXPECT_EQ(guarded.front(), -999);
            EXPECT_EQ(guarded[available + 1], -999);
        }
    });
    start.set_value();
    for (int block = 0; block < 1000; ++block) {
        supply(synth, 31, block * 31);
        synth.waitProcessed();
    }
    done = true;
    controls.join();
    consumer.join();
    synth.endAudioRenderingThread();
    const auto stats = synth.getBufferStatistics();
    EXPECT_EQ(delivered.load(), 31001U);
    EXPECT_EQ(stats.rendered, stats.delivered);
    EXPECT_EQ(stats.droppedInput, 0U);
    EXPECT_EQ(stats.droppedOutput, 0U);
    EXPECT_EQ(stats.zeroFilledOutput, 0U);
    EXPECT_EQ(stats.completedGeneration, 1000U);
}

TEST(ThreadedSynthesis, PausedConsumerStopAndNewestBlockDiscard) {
    Synthesizer synth;
    setup(synth, 1);
    synth.startAudioRenderingThread();
    supply(synth, 32);
    ASSERT_TRUE(until([&] { const auto s = synth.getBufferStatistics(); return s.rendering && s.queuedOutput == 1; }));
    supply(synth, 32, 32);
    supply(synth, 32, 64);
    EXPECT_EQ(synth.getDroppedInputSamples(), 32U);
    synth.endAudioRenderingThread();
    synth.endAudioRenderingThread();
    const auto stats = synth.getBufferStatistics();
    EXPECT_EQ(stats.rendered, 65U);
    EXPECT_EQ(stats.droppedOutput, 64U);
    EXPECT_EQ(stats.queuedOutput, 1U);
    EXPECT_EQ(stats.completedGeneration, 2U);
    EXPECT_EQ(stats.submittedGeneration, 2U);
    EXPECT_EQ(stats.pendingInput, 0U);
    EXPECT_EQ(stats.activeInput, 0U);
    std::array<int16_t, 6> output;
    output.fill(-999);
    EXPECT_EQ(synth.readAudioOutput(4, output.data() + 1), 1);
    EXPECT_EQ(output.front(), -999);
    EXPECT_EQ(output.back(), -999);
    EXPECT_EQ(output[2], 0);
    EXPECT_EQ(output[3], 0);
    EXPECT_EQ(output[4], 0);
    EXPECT_EQ(synth.getBufferStatistics().zeroFilledOutput, 3U);
}

TEST(ThreadedSynthesis, FullOutputDrainWakesPublisher) {
    Synthesizer synth;
    setup(synth, 31);
    synth.startAudioRenderingThread();
    supply(synth, 256);
    ASSERT_TRUE(until([&] { return synth.getBufferStatistics().queuedOutput == 31; }));
    size_t count = 0;
    EXPECT_TRUE(until([&] {
        std::array<int16_t, 31> output;
        const auto available = synth.getBufferStatistics().queuedOutput;
        if (available != 0) count += synth.readAudioOutput(static_cast<int>(available), output.data());
        return synth.getBufferStatistics().completedGeneration == 1;
    }));
    auto stats = synth.getBufferStatistics();
    std::array<int16_t, 31> output;
    if (stats.queuedOutput != 0) count += synth.readAudioOutput(static_cast<int>(stats.queuedOutput), output.data());
    synth.endAudioRenderingThread();
    EXPECT_EQ(count, 257U);
    EXPECT_EQ(synth.getDroppedOutputSamples(), 0U);
}

TEST(ThreadedSynthesis, StopDuringProcessingAndIdleRestart) {
    Synthesizer synth;
    auto p = parameters(8192);
    p.inputBufferSize = 8192;
    synth.initialize(p);
    std::vector<int16_t> impulse(10000, 101);
    impulse[0] = INT16_MAX;
    synth.initializeImpulseResponse(impulse.data(), static_cast<unsigned>(impulse.size()), 1, 0);
    synth.startAudioRenderingThread();
    EXPECT_THROW(synth.initializeImpulseResponse(impulse.data(), 1, 1, 0), std::logic_error);
    supply(synth, 4096);
    EXPECT_TRUE(until([&] { return synth.getBufferStatistics().rendering; }));
    std::thread stopper([&] { synth.endAudioRenderingThread(); });
    for (int block = 0; block < 3; ++block) supply(synth, 31, 4096 + block * 31);
    stopper.join();
    synth.renderAudio(); // A block submitted after stop returned can be drained synchronously.
    const auto stats = synth.getBufferStatistics();
    EXPECT_EQ(stats.rendered + stats.droppedInput, 4190U);
    EXPECT_EQ(stats.queuedOutput, stats.rendered);
    EXPECT_EQ(stats.droppedOutput, 0U);
    EXPECT_EQ(stats.completedGeneration, stats.submittedGeneration);
    EXPECT_GE(stats.completedGeneration, 1U);
    for (int repeat = 0; repeat < 50; ++repeat) {
        synth.startAudioRenderingThread();
        synth.endInputBlock();
        synth.waitProcessed();
        synth.endAudioRenderingThread();
        synth.endAudioRenderingThread();
    }
    EXPECT_EQ(synth.getBufferStatistics().rendered, stats.rendered);
}
