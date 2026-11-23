#ifndef ENGINE_SIM_SYNTHESIS_TEST_SCENARIO_HPP
#define ENGINE_SIM_SYNTHESIS_TEST_SCENARIO_HPP

#include "synthesizer.h"

#include <array>
#include <cstdint>

namespace synthesis_test {
using Input = std::array<double, 2>;
constexpr std::uint32_t Frequency = 24000;
constexpr std::size_t Frames = 24011;
constexpr std::size_t NativeFrames = 8192;

inline Input excitation(std::size_t index) {
    if (index == 0) return {83.5, -24.25};
    if (index == 9) return {-23.0, 128.0};
    if (index < 32) return {};
    if (index < 1700) {
        const auto n = static_cast<int>(index);
        return {(n % 121 - 60) * 1.25, (n % 113 - 56) * 0.75};
    }
    if (index < 1870) {
        const auto direction = index / 16 % 2 == 0 ? 1.0 : -1.0;
        return {direction * 4e8, -direction * 8e7};
    }
    if (index < 3500) return {0.75, -0.625};
    return {};
}

inline void prepare(Synthesizer &synth, std::uint64_t seed, bool exchangeResponses = false) {
    constexpr std::array<std::int16_t, 7> first{12000, -3040, 481, 1110, -202, 145, 50};
    constexpr std::array<std::int16_t, 5> second{-8000, 13000, 2400, 2421, 100};
    Synthesizer::Parameters params;
    params.inputChannelCount = 2;
    params.inputBufferSize = 257;
    params.audioBufferSize = 511;
    params.inputSampleRate = Frequency;
    params.audioSampleRate = 44100;
    params.seed = seed;
    synth.initialize(params);
    // Keep the old prepare order: set mixer after oscillator and queue setup.
    auto audio = synth.getAudioParameters();
    audio.volume = 1.95f;
    audio.convolution = 0.771f;
    audio.dF_F_mix = 0.029f;
    audio.airNoise = 0.423f;
    audio.inputSampleNoise = 0.201f;
    synth.setAudioParameters(audio);
    synth.initializeImpulseResponse(first.data(), static_cast<unsigned>(first.size()), 0.35f, exchangeResponses ? 1 : 0);
    synth.initializeImpulseResponse(second.data(), static_cast<unsigned>(second.size()), 0.77f, exchangeResponses ? 0 : 1);
}

inline void changeMixer(Synthesizer &synth, std::size_t frame) {
    if (frame != 777 && frame != 1001) return;
    auto audio = synth.getAudioParameters();
    audio.airNoise = frame == 777 ? 0.0f : 0.423f;
    audio.inputSampleNoise = frame == 777 ? 0.0f : 0.201f;
    synth.setAudioParameters(audio);
}
}

#endif
