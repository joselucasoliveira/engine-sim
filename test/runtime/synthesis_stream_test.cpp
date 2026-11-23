#include "synthesis_scenario.hpp"
#include "synthesis_trace.hpp"
#include "sample_clock.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
using namespace engine_sim;
using namespace synthesis_test;

class Source : public ClockSource {
public:
    explicit Source(std::uint64_t seed) {
        prepare(m_synth, seed);
        m_synth.beginSynchronous();
    }
    void latch() override {}

    bool tick(double *endpoint, std::size_t capacity) override {
        if (capacity < 2 || endpoint == nullptr) return false;
        const auto next = excitation(m_ticks++);
        std::copy(next.begin(), next.end(), endpoint);
        return true;
    }

    bool native(AcousticExcitation input, std::int16_t &sample) override {
        if (input.count != 2) return false;
        const auto frame = nativeSamples.size();
        const auto completed = frame * Frequency / SampleClock::NativeRate;
        const auto fraction = static_cast<double>(frame * Frequency % SampleClock::NativeRate) / SampleClock::NativeRate;
        const auto older = completed >= 2 ? excitation(completed - 2) : Input{};
        const auto newer = completed >= 1 ? excitation(completed - 1) : Input{};
        changeMixer(m_synth, frame);
        sample = m_synth.renderNativeFrame(input);
        nativeSamples.push_back(sample);
        trace.write(static_cast<std::uint16_t>(sample), 2);
        for (std::size_t channel = 0; channel < input.count; ++channel) {
            const auto expected = (1 - fraction) * older[channel] + fraction * newer[channel];
            const auto observed = bits<std::uint64_t>(input.data[channel]);
            require(observed == bits<std::uint64_t>(expected), "Native input differs from the absolute interpolation equation");
            trace.write(observed, 8);
        }
        return true;
    }
    std::vector<std::int16_t> nativeSamples;
    Trace trace;

private:
    Synthesizer m_synth;
    std::size_t m_ticks = 0;
};

Trace render(std::uint32_t rate, std::uint64_t seed, const std::vector<std::size_t> &partitions) {
    Source source(seed);
    SampleClock clock(Frequency, {0., 0.}, source, SampleClock::Profile::Public, rate);
    std::vector<float> output(Frames);
    std::size_t part = 0;
    for (std::size_t frame = 0; frame < output.size();) {
        const auto size = std::min(partitions[part++ % partitions.size()], output.size() - frame);
        clock.render(output.data() + frame, size);
        frame += size;
    }
    for (std::size_t frame = 0; frame < output.size(); ++frame) {
        const auto index = frame * SampleClock::NativeRate / rate;
        float sample;
        if (rate == SampleClock::NativeRate) {
            sample = static_cast<float>(source.nativeSamples.at(index)) / 32768.0f;
        } else {
            const auto fraction = static_cast<double>(frame * SampleClock::NativeRate % rate) / rate;
            const auto previous = index == 0 ? 0 : index - 1;
            sample = static_cast<float>(((1 - fraction) * source.nativeSamples.at(previous) +
                                         fraction * source.nativeSamples.at(index)) / 32768.0);
        }
        const auto word = bits<std::uint32_t>(output[frame]);
        require(word == bits<std::uint32_t>(sample), "Output differs from the causal native sample equation");
        source.trace.write(word, 4);
    }
    const auto counts = clock.counts();
    require(counts.frames == Frames && counts.ticks == Frames * Frequency / rate &&
            counts.nativeFrames == (Frames * SampleClock::NativeRate + rate - 1) / rate &&
            source.nativeSamples.size() == counts.nativeFrames, "Native clock counters differ");
    source.trace.write(counts.frames, 8);
    source.trace.write(counts.ticks, 8);
    source.trace.write(counts.nativeFrames, 8);
    return std::move(source.trace);
}
}

int main() {
    try {
        for (const auto rate : {44100u, 48000u}) {
            for (const auto seed : {403u, 404u}) {
                const auto unit = render(rate, seed, {1});
                const auto irregular = render(rate, seed, {7, 251, 31, 1, 503});
                const auto maximum = render(rate, seed, {SampleClock::MaximumBlock});
                require(unit.bytes == irregular.bytes && unit.bytes == maximum.bytes,
                        "Output partition changed the ordered stream or PCM");
                if (const auto *directory = std::getenv("ENGINE_SIM_GATE_OUTPUT")) {
                    const auto name = "rate-" + std::to_string(rate) + "-seed-" + std::to_string(seed) + ".raw";
                    unit.save(std::filesystem::u8path(directory) / name);
                }
            }
        }
        std::cout << "Compared native samples, interpolated inputs and output PCM at both rates\n";
        return 0;
    }
    catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
