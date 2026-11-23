#include "synthesis_scenario.hpp"
#include "synthesis_trace.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>

namespace {
using namespace synthesis_test;

Trace exercise(std::uint64_t seed, bool swapResponses, bool invalidInputs) {
    Synthesizer synth;
    prepare(synth, seed, swapResponses);
    synth.beginSynchronous();
    if (invalidInputs) {
        std::array<double, 3> zeros{};
        for (const auto invalid : {engine_sim::AcousticExcitation{nullptr, 2}, {zeros.data(), 1}, {zeros.data(), 3}}) {
            bool rejected = false;
            try { synth.renderNativeFrame(invalid); }
            catch (const std::invalid_argument &) { rejected = true; }
            require(rejected, "Native synthesis accepted a malformed input extent");
        }
        require(synth.getBufferStatistics().rendered == 0, "Rejected span advanced the synth");
        bool rejected = false;
        try { synth.startAudioRenderingThread(); }
        catch (const std::logic_error &) { rejected = true; }
        require(rejected, "Synchronous synthesis allowed a worker");
    }
    std::array<engine_sim::RandomStream, 2> streams{
        engine_sim::RandomStream(seed, 2, 0), engine_sim::RandomStream(seed, 2, 1)
    };
    Trace trace;
    auto min = std::numeric_limits<std::int16_t>::max();
    auto max = std::numeric_limits<std::int16_t>::min();
    for (std::size_t i = 0; i < NativeFrames; ++i) {
        const auto input = excitation(i);
        changeMixer(synth, i);
        const auto sample = synth.renderNativeFrame({input.data(), input.size()});
        min = std::min(min, sample);
        max = std::max(max, sample);
        if (i > NativeFrames - 128) require(sample == 0, "Asymmetric input did not decay to a silent tail");
        trace.write(static_cast<std::uint16_t>(sample), 2);
        trace.write(bits<std::uint32_t>(synth.m_levelingFilter.getAttenuation()), 4);
        for (std::size_t channel = 0; channel < input.size(); ++channel) {
            (void)streams[channel].next();
            auto expected = streams[channel];
            auto consumed = synth.m_filters[channel].airNoise;
            const auto next = consumed.next();
            require(next == expected.next(), "Seeded channel skipped or repeated an air noise draw");
            trace.write(next, 8);
        }
    }
    require(min == std::numeric_limits<std::int16_t>::min(), "Negative native saturation did not occur");
    require(max == std::numeric_limits<std::int16_t>::max(), "Positive native saturation did not occur");
    const auto queues = synth.getBufferStatistics();
    require(queues.rendered == NativeFrames && !queues.rendering && queues.queuedOutput == 0 &&
            queues.pendingInput == 0 && queues.stagedInput == 0 && synth.m_thread == nullptr,
            "Native rendering entered the legacy worker or queues");
    return trace;
}
}

int main(int argc, char **argv) {
    try {
        require(argc == 1 || argc == 2, "Expected at most one evidence directory");
        const auto first = exercise(403, false, false);
        const auto invalid = exercise(403, false, true);
        const auto second = exercise(404, false, false);
        const auto swapped = exercise(403, true, false);
        require(first.bytes == invalid.bytes, "Rejected extents changed synthesis history");
        require(first.bytes != swapped.bytes, "Swapping amplified impulse responses failed to change synthesis");
        bool seedChangedPcm = false;
        for (std::size_t i = 0; i < first.bytes.size(); i += 22) {
            if (first.bytes[i] != second.bytes[i] || first.bytes[i + 1] != second.bytes[i + 1]) {
                seedChangedPcm = true;
                break;
            }
        }
        require(seedChangedPcm, "Changing the seed failed to change PCM");
        if (argc == 2) {
            const auto path = std::filesystem::u8path(argv[1]);
            first.save(path / "rate-0-seed-403.raw");
            second.save(path / "rate-0-seed-404.raw");
        }
        std::cout << "Checked " << NativeFrames << " native samples per seed, saturation, stream and tail history\n";
        return 0;
    }
    catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
