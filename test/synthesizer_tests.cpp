#include <gtest/gtest.h>

#include "../include/synthesizer.h"
#include "../include/audio_buffer.h"
#include "../include/delay_filter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace {
Synthesizer::Parameters parameters(int capacity = 2048) {
    Synthesizer::Parameters p;
    p.inputBufferSize = capacity;
    p.audioBufferSize = 4096;
    p.inputSampleRate = p.audioSampleRate = 44100;
    p.initialAudioParameters.airNoise = 0;
    p.initialAudioParameters.inputSampleNoise = 0;
    p.initialAudioParameters.levelerMaxGain = 1;
    p.initialAudioParameters.levelerMinGain = 1;
    p.initialAudioParameters.dF_F_mix = 0;
    return p;
}

void setupSynchronizedSynthesizer(Synthesizer &synth) {
    synth.initialize(parameters());
    const int16_t impulse[] = {INT16_MAX};
    synth.initializeImpulseResponse(impulse, 1, 1, 0);
}

bool containsSignal(const int16_t *begin, const int16_t *end) {
    return std::any_of(begin, end, [](int16_t value) { return value != 0; });
}
}

TEST(RingBuffer, CapacityWrapAndOverflow) {
    for (size_t capacity : {1U, 31U, 32U}) {
        RingBuffer<int> queue;
        EXPECT_THROW(queue.initialize(0), std::invalid_argument);
        queue.initialize(capacity);
        EXPECT_EQ(queue.size(), 0U);
        EXPECT_THROW(queue.read(0), std::out_of_range);
        for (int cycle = 0; cycle < 5; ++cycle) {
            for (size_t i = 0; i < capacity; ++i) ASSERT_TRUE(queue.write(cycle * 100 + static_cast<int>(i)));
            EXPECT_EQ(queue.size(), capacity);
            EXPECT_FALSE(queue.write(-1));
            EXPECT_EQ(queue.read(0), cycle * 100);
            EXPECT_THROW(queue.overwrite(-1, capacity), std::out_of_range);
            std::vector<int> guarded(capacity + 3, -999);
            EXPECT_EQ(queue.readAndRemove(capacity + 1, guarded.data() + 1), capacity);
            EXPECT_EQ(guarded.front(), -999);
            EXPECT_EQ(guarded[capacity + 1], -999);
            EXPECT_EQ(guarded.back(), -999);
            for (size_t i = 0; i < capacity; ++i) EXPECT_EQ(guarded[i + 1], cycle * 100 + static_cast<int>(i));
            EXPECT_EQ(queue.size(), 0U);
            EXPECT_EQ(queue.removeBeginning(capacity + 1), 0U);
            // Move both cursors so the next full transfer crosses the allocation end.
            ASSERT_TRUE(queue.write(0));
            queue.removeBeginning(1);
        }
        EXPECT_EQ(queue.index(0, -static_cast<int>(capacity) * 3 - 1), capacity - 1);
        queue.initialize(capacity);
        EXPECT_EQ(queue.size(), 0U);
    }
}

TEST(RingBuffer, AudioBufferFullCopyAndGuardedExtents) {
    AudioBuffer audio;
    EXPECT_THROW(audio.initialize(0, 32), std::invalid_argument);
    EXPECT_THROW(audio.initialize(44100, 0), std::invalid_argument);
    audio.initialize(44100, 32);
    for (int i = 0; i < 32; ++i) audio.writeSample(i + 1, i);
    for (int offset : {0, 1, 31, -1}) {
        for (int count : {0, 1, 31, 32}) {
            std::array<int16_t, 34> target;
            target.fill(-999);
            audio.copyBuffer(target.data() + 1, offset, count);
            EXPECT_EQ(target.front(), -999);
            for (int i = 0; i < count; ++i) EXPECT_EQ(target[i + 1], ((offset + i + 32) % 32) + 1);
            for (int i = count + 1; i < 34; ++i) EXPECT_EQ(target[i], -999);
        }
    }
    EXPECT_THROW(audio.copyBuffer(nullptr, 0, 1), std::invalid_argument);
    EXPECT_THROW(audio.copyBuffer(nullptr, 0, 33), std::out_of_range);
    EXPECT_THROW(audio.commitBlock(-1), std::out_of_range);
    audio.commitBlock(32);
    EXPECT_EQ(audio.m_writePointer, 0);
    audio.destroy();
    audio.destroy();
}

TEST(RingBuffer, FixedHistoryAndDelayRemainDefined) {
    ButterworthLowPassFilter<double> filter;
    filter.setCutoffFrequency(1900, 44100);
    std::array<double, 32> first;
    for (size_t i = 0; i < first.size(); ++i) first[i] = filter.fast_f(i == 0 ? 1.0 : 0.0);
    EXPECT_GT(*std::max_element(first.begin(), first.end()), 0.01);
    filter.resetHistory();
    for (size_t i = 0; i < first.size(); ++i) EXPECT_DOUBLE_EQ(filter.fast_f(i == 0 ? 1.0 : 0.0), first[i]);
    for (int latency : {0, 1, 31, 32}) {
        DelayFilter delay;
        delay.initialize(latency / 44100.0, 44100);
        for (int i = 0; i < 100; ++i) EXPECT_DOUBLE_EQ(delay.fast_f(i + 1), i < latency ? 0 : i - latency + 1);
    }
}

TEST(Convolution, AnalyticTapsEmptyAndReplacement) {
    ConvolutionFilter filter;
    EXPECT_FLOAT_EQ(filter.f(123), 0);
    EXPECT_THROW(filter.initialize(-1), std::invalid_argument);
    filter.initialize(4);
    const float taps[] = {1, -0.5f, 0.25f, 0.125f};
    std::copy(std::begin(taps), std::end(taps), filter.getImpulseResponse());
    for (int i = 0; i < 20; ++i) EXPECT_FLOAT_EQ(filter.f(i == 0 ? 1 : 0), i < 4 ? taps[i] : 0);
    filter.initialize(1);
    filter.getImpulseResponse()[0] = 0.5f;
    EXPECT_FLOAT_EQ(filter.f(4), 2);
    filter.initialize(0);
    EXPECT_EQ(filter.getSampleCount(), 0);
    EXPECT_FLOAT_EQ(filter.f(123), 0);
}

TEST(Convolution, ChannelsTrimAndInputValidation) {
    Synthesizer synth;
    synth.initialize(parameters());
    EXPECT_THROW(synth.initializeImpulseResponse(nullptr, 1, 1, 0), std::invalid_argument);
    EXPECT_THROW(synth.initializeImpulseResponse(nullptr, 0, 1, -1), std::out_of_range);
    EXPECT_THROW(synth.initializeImpulseResponse(nullptr, 0, 1, 1), std::out_of_range);
    const int16_t silent[] = {0, 100, -100, 0};
    synth.initializeImpulseResponse(silent, 4, 1, 0);
    EXPECT_EQ(synth.m_filters[0].convolution.getSampleCount(), 0);
    EXPECT_FLOAT_EQ(synth.m_filters[0].convolution.f(100), 0);
    std::vector<int16_t> longResponse(10032, 101);
    synth.initializeImpulseResponse(longResponse.data(), static_cast<unsigned>(longResponse.size()), 1, 0);
    EXPECT_EQ(synth.m_filters[0].convolution.getSampleCount(), 10000);
    const int16_t unit[] = {INT16_MAX, 0};
    synth.initializeImpulseResponse(unit, 2, 0.5f, 0);
    EXPECT_EQ(synth.m_filters[0].convolution.getSampleCount(), 1);
    EXPECT_FLOAT_EQ(synth.m_filters[0].convolution.f(100), 50);
    synth.initializeImpulseResponse(nullptr, 0, 1, 0);
    EXPECT_FLOAT_EQ(synth.m_filters[0].convolution.f(100), 0);
}

TEST(SynthesizerTests, SynthesizerSanityCheck) {
    Synthesizer synth;
    auto p = parameters();
    p.inputChannelCount = 0;
    EXPECT_THROW(synth.initialize(p), std::invalid_argument);
    p = parameters();
    p.audioBufferSize = 0;
    EXPECT_THROW(synth.initialize(p), std::invalid_argument);
    p = parameters();
    p.inputSampleRate = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(synth.initialize(p), std::invalid_argument);
    setupSynchronizedSynthesizer(synth);
    EXPECT_THROW(synth.setInputSampleRate(0), std::invalid_argument);
    EXPECT_THROW(synth.readAudioOutput(-1, nullptr), std::invalid_argument);
    EXPECT_THROW(synth.readAudioOutput(1, nullptr), std::invalid_argument);
    EXPECT_EQ(synth.readAudioOutput(0, nullptr), 0);
    std::array<int16_t, 34> output;
    output.fill(-999);
    EXPECT_EQ(synth.readAudioOutput(32, output.data() + 1), 0);
    EXPECT_EQ(output.front(), -999);
    EXPECT_EQ(output.back(), -999);
    for (int i = 1; i <= 32; ++i) EXPECT_EQ(output[i], 0);
    synth.renderAudio();
}

TEST(SynthesizerTests, SynthesizerSystemTestSingleThread) {
    Synthesizer synth;
    setupSynchronizedSynthesizer(synth);
    std::array<int16_t, 67> guarded;
    guarded.fill(-999);
    int total = 0;
    for (int i = 0; i < 64;) {
        for (int j = 0; j < 16; ++j, ++i) {
            const double input[] = {i == 16 ? 8192.0 : 0};
            synth.writeInput(input);
        }
        synth.endInputBlock();
        synth.renderAudio();
        const int count = i == 16 ? 17 : 16;
        ASSERT_EQ(synth.readAudioOutput(count, guarded.data() + 1 + total), count);
        total += count;
    }
    EXPECT_EQ(total, 65); // Explicit initial phase emits t=0 and the first input endpoint.
    EXPECT_EQ(guarded.front(), -999);
    EXPECT_EQ(guarded.back(), -999);
    EXPECT_TRUE(containsSignal(guarded.data() + 1, guarded.data() + 66));
    EXPECT_EQ(synth.getDroppedInputSamples(), 0U);
    EXPECT_EQ(synth.getDroppedOutputSamples(), 0U);
}

TEST(SynthesizerTests, SynthesizerSystemTest) {
    Synthesizer synth;
    setupSynchronizedSynthesizer(synth);
    synth.startAudioRenderingThread();
    std::array<int16_t, 10> idle;
    idle.fill(-999);
    for (int i = 0; i < 1024; ++i) {
        const double input[] = {i == 16 ? 8192.0 : 0};
        synth.writeInput(input);
        if (i % 16 == 0) EXPECT_EQ(synth.readAudioOutput(8, idle.data() + 1), 0);
    }
    synth.endInputBlock();
    synth.waitProcessed();
    // Join before inspecting output. Concurrent consumption and completion visibility
    // have a dedicated handoff regression in the next synchronization change.
    synth.endAudioRenderingThread();
    std::array<int16_t, 1029> output;
    output.fill(-999);
    EXPECT_EQ(synth.readAudioOutput(1027, output.data() + 1), 1025);
    EXPECT_EQ(output.front(), -999);
    EXPECT_EQ(output.back(), -999);
    EXPECT_EQ(output[1026], 0);
    EXPECT_EQ(output[1027], 0);
    EXPECT_EQ(idle.front(), -999);
    EXPECT_EQ(idle.back(), -999);
    EXPECT_TRUE(containsSignal(output.data() + 1, output.data() + 1026));
}

TEST(SynthesizerTests, ResamplingPhaseAndDiscardNewestBlock) {
    Synthesizer synth;
    auto p = parameters(32);
    p.inputSampleRate = 22050;
    synth.initialize(p);
    const double a[] = {100}, b[] = {200};
    synth.writeInput(a);
    synth.writeInput(b);
    ASSERT_EQ(synth.m_inputChannels[0].data.size(), 5U);
    ButterworthLowPassFilter<double> oracle;
    oracle.setCutoffFrequency(1900.0f, 44100.0f);
    const float expected[] = {0, 50, 100, 150, 200};
    for (size_t i = 0; i < 5; ++i) EXPECT_FLOAT_EQ(synth.m_inputChannels[0].data.read(i), oracle.fast_f(expected[i]));
    for (int i = 0; i < 13; ++i) synth.writeInput(b);
    ASSERT_EQ(synth.m_inputChannels[0].data.size(), 31U);
    const float oldest = synth.m_inputChannels[0].data.read(0);
    synth.writeInput(b); // Two interpolated frames cannot fit in the one remaining slot.
    EXPECT_EQ(synth.getDroppedInputSamples(), 2U);
    EXPECT_EQ(synth.m_inputChannels[0].data.size(), 31U);
    EXPECT_FLOAT_EQ(synth.m_inputChannels[0].data.read(0), oldest);
    synth.endInputBlock();
    synth.renderAudio();
    std::array<int16_t, 33> output;
    output.fill(-999);
    EXPECT_EQ(synth.readAudioOutput(31, output.data() + 1), 31);
    EXPECT_EQ(output.front(), -999);
    EXPECT_EQ(output.back(), -999);
}

TEST(SynthesizerTests, EmptyWetUnitResponseDryGainAndClipping) {
    std::array<std::vector<int16_t>, 3> rendered;
    for (int mode = 0; mode < 3; ++mode) {
        Synthesizer synth;
        auto p = parameters();
        p.initialAudioParameters.convolution = mode == 2 ? 0 : 1;
        synth.initialize(p);
        if (mode == 1) {
            const int16_t unit[] = {INT16_MAX};
            synth.initializeImpulseResponse(unit, 1, 1, 0);
        }
        for (int i = 0; i < 256; ++i) {
            const double input[] = {i < 128 ? 1e9 : -1e9};
            synth.writeInput(input);
        }
        synth.endInputBlock();
        synth.renderAudio();
        rendered[mode].resize(257);
        EXPECT_EQ(synth.readAudioOutput(257, rendered[mode].data()), 257);
    }
    EXPECT_FALSE(containsSignal(rendered[0].data(), rendered[0].data() + 257));
    EXPECT_EQ(rendered[1], rendered[2]);
    EXPECT_NE(std::find(rendered[1].begin(), rendered[1].end(), INT16_MAX), rendered[1].end());
    EXPECT_NE(std::find(rendered[1].begin(), rendered[1].end(), INT16_MIN), rendered[1].end());
}

TEST(SynthesizerTests, SmallOutputCapacityAndLargeInputStep) {
    for (int capacity : {1, 31, 32}) {
        Synthesizer synth;
        auto p = parameters(64);
        p.audioBufferSize = capacity;
        synth.initialize(p);
        const double input[] = {1};
        for (int i = 0; i < std::max(1, capacity - 1); ++i) synth.writeInput(input);
        synth.endInputBlock();
        synth.renderAudio();
        EXPECT_EQ(synth.m_audioBuffer.size(), static_cast<size_t>(capacity));
        synth.endInputBlock();
        synth.renderAudio(); // Full output cannot be overwritten or block its caller.
        EXPECT_EQ(synth.m_audioBuffer.size(), static_cast<size_t>(capacity));
        std::vector<int16_t> guarded(capacity + 2, -999);
        EXPECT_EQ(synth.readAudioOutput(capacity, guarded.data() + 1), capacity);
        EXPECT_EQ(guarded.front(), -999);
        EXPECT_EQ(guarded.back(), -999);
        EXPECT_EQ(synth.getDroppedOutputSamples(), 0U);
    }
    Synthesizer synth;
    auto p = parameters(1);
    synth.initialize(p);
    const double input[] = {1};
    synth.writeInput(input); // Initial two-frame block is too large for a single slot.
    EXPECT_EQ(synth.getDroppedInputSamples(), 2U);
    synth.writeInput(input); // Phase advances even though the preceding block was discarded.
    EXPECT_EQ(synth.m_inputChannels[0].data.size(), 1U);
    EXPECT_EQ(synth.getDroppedInputSamples(), 2U);
    synth.destroy();
    p = parameters(32);
    p.inputSampleRate = p.audioSampleRate / 64;
    p.inputChannelCount = 2;
    synth.initialize(p);
    const double stereo[] = {1, 2};
    synth.writeInput(stereo);
    EXPECT_EQ(synth.getDroppedInputSamples(), 65U);
    EXPECT_EQ(synth.m_inputChannels[0].data.size(), 0U);
    EXPECT_EQ(synth.m_inputChannels[1].data.size(), 0U);
    synth.endInputBlock();
    synth.renderAudio();
}

TEST(SynthesizerTests, DryVolumeGain) {
    std::array<std::vector<int16_t>, 2> rendered;
    for (int mode = 0; mode < 2; ++mode) {
        Synthesizer synth;
        auto p = parameters();
        p.initialAudioParameters.convolution = 0;
        p.initialAudioParameters.volume = mode == 0 ? 1 : 0.5f;
        synth.initialize(p);
        for (int i = 0; i < 256; ++i) {
            const double input[] = {i < 128 ? 1024.0 : -1024.0};
            synth.writeInput(input);
        }
        synth.endInputBlock();
        synth.renderAudio();
        rendered[mode].resize(257);
        EXPECT_EQ(synth.readAudioOutput(257, rendered[mode].data()), 257);
    }
    EXPECT_TRUE(containsSignal(rendered[0].data(), rendered[0].data() + 257));
    for (size_t i = 0; i < 257; ++i) EXPECT_NEAR(rendered[1][i], rendered[0][i] * 0.5, 0.5);
}
