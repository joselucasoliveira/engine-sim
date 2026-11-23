#ifndef ENGINE_SIM_CAPTURE_STRESS_H
#define ENGINE_SIM_CAPTURE_STRESS_H

#include "../../src/runtime/capture_runtime.hpp"
#include "input_evidence.h"
#include "ring_buffer.h"
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <thread>

namespace engine_sim_capture {
inline void stressRequire(bool valid, const char *message) {
    if (!valid) throw std::runtime_error(message);
}

inline void stressQueue(const std::filesystem::path &out) {
    RingBuffer<int> ring;
    ring.initialize(3);
    for (int round = 0; round < 1000; ++round) {
        const int base = round * 4;
        for (int i = 0; i < 3; ++i) stressRequire(ring.write(base + i), "queue rejected available capacity");
        stressRequire(!ring.write(-1), "queue accepted overflow");
        std::array<int, 4> values{-1, -1, -1, -1};
        stressRequire(ring.readAndRemove(2, values.data() + 1) == 2
            && values == std::array<int, 4>{-1, base, base + 1, -1}, "queue corrupted its guarded destination");
        stressRequire(ring.write(base + 3), "wrapped queue rejected available capacity");
        stressRequire(ring.readAndRemove(2, values.data() + 1) == 2
            && values == std::array<int, 4>{-1, base + 2, base + 3, -1} && ring.size() == 0,
            "wrapped queue lost sample order");
    }
    std::ofstream(out / "stress.json") << "{\"version\":2,\"scope\":\"primitive-ring\",\"case\":\"QUEUE\",\"operations\":4000,\"cycles\":1000,\"rejected\":1000,\"valid\":true}\n";
}

inline void stressThread(const std::filesystem::path &out, std::uint64_t seed = 1) {
    Synthesizer synth;
    Synthesizer::Parameters p;
    p.seed = seed;
    p.inputSampleRate = p.audioSampleRate = 44100;
    p.inputBufferSize = 512; p.audioBufferSize = 64;
    p.initialAudioParameters.inputSampleNoise = p.initialAudioParameters.airNoise = 0;
    synth.initialize(p);
    const int16_t impulse = INT16_MAX;
    synth.initializeImpulseResponse(&impulse, 1, 1, 0);
    synth.startAudioRenderingThread();
    std::atomic<bool> done{false}, valid{true};
    std::atomic<unsigned> updates{0}, delivered{0};
    std::promise<void> firstUpdate;
    auto updated = firstUpdate.get_future();
    std::thread controls([&] {
        do {
            auto audio = synth.getAudioParameters();
            audio.volume = updates % 2 ? 0.5f : 1.0f;
            synth.setAudioParameters(audio);
            if (++updates == 1) firstUpdate.set_value();
        } while (!done);
    });
    struct Join {
        std::atomic<bool> &done;
        std::thread &thread;
        ~Join() { done = true; if (thread.joinable()) thread.join(); }
    } joinControls{done, controls};
    std::thread consumer([&] {
        while (!done || synth.getBufferStatistics().queuedOutput != 0) {
            const auto available = synth.getBufferStatistics().queuedOutput;
            if (available == 0) { std::this_thread::yield(); continue; }
            std::array<int16_t, 66> guarded;
            guarded.fill(-999);
            const auto count = synth.readAudioOutput(static_cast<int>(available), guarded.data() + 1);
            delivered += count;
            if (guarded.front() != -999 || guarded[available + 1] != -999) valid = false;
        }
    });
    Join joinConsumer{done, consumer};
    updated.wait();
    for (int block = 0; block < 1000; ++block) {
        for (int i = 0; i < 31; ++i) { const double value = (i % 2 ? 1 : -1) * 2048; synth.writeInput(&value); }
        synth.endInputBlock();
        synth.waitProcessed();
    }
    done = true;
    controls.join(); consumer.join();
    synth.endAudioRenderingThread();
    const auto stats = synth.getBufferStatistics();
    stressRequire(valid && updates > 0 && delivered == 31001 && stats.rendered == stats.delivered
        && stats.completedGeneration == 1000 && stats.submittedGeneration == 1000
        && stats.droppedInput == 0 && stats.droppedOutput == 0 && stats.zeroFilledOutput == 0,
        "threaded stress lost publication, samples or destination guards");
    for (int cycle = 0; cycle < 50; ++cycle) { synth.startAudioRenderingThread(); synth.endAudioRenderingThread(); }
    std::ofstream(out / "stress.json") << "{\"version\":2,\"scope\":\"legacy-synth-worker\",\"case\":\"THREAD\",\"operations\":1000,\"cycles\":50,\"frames\":"
        << delivered << ",\"control_updates\":" << updates << ",\"valid\":true}\n";
}

inline void stressLifecycle(const std::filesystem::path &resources, const std::filesystem::path &out,
        std::uint64_t seed = 1) {
    InputEvidence inputs(out);
    const auto invalidEntry = out / "stress-invalid.json";
    { std::ofstream file(invalidEntry); file << "{\"format_version\":1,\"engine\":null}\n"; }
    std::unique_ptr<RuntimeHost> active;
    int frames = 0;
    for (int cycle = 0; cycle < 10; ++cycle) {
        const auto entry = resources / "assets/engines" /
            (cycle % 2 ? "atg-video-2/09_radial_9.json" : "atg-video-1/02_kohler_ch750.json");
        auto candidate = RuntimeHost::prepare(entry, resources, seed);
        inputs.append(*candidate);
        active.swap(candidate);
        stressRequire(active->engine().getCylinderCount() == (cycle % 2 ? 9 : 2), "lifecycle topology mismatch");
        active->setControls({0, 0, true, true, false});
        active->beginBlock();
        std::array<float, 441> pcm{};
        std::uint32_t count = 0;
        active->render(pcm.data(), pcm.size(), count);
        active->endBlock();
        stressRequire(count == pcm.size(), "lifecycle produced incomplete PCM");
        frames += count;
        auto *original = active.get();
        const auto before = active->telemetry();
        bool rejected = false;
        try { auto invalid = RuntimeHost::prepare(invalidEntry, resources, seed); active.swap(invalid); }
        catch (const EngineLoadError &) { rejected = true; }
        stressRequire(rejected && active.get() == original && active->telemetry().frames == before.frames
            && active->telemetry().ticks == before.ticks, "failed reload replaced or advanced the running session");
        active->beginBlock(); count = 0;
        active->render(pcm.data(), pcm.size(), count); active->endBlock();
        stressRequire(count == pcm.size() && active->telemetry().frames == 882, "failed reload damaged continuation");
        frames += count;
        std::cout << "completed lifecycle replacement " << cycle + 1 << " of 10\n" << std::flush;
    }
    active.reset();
    std::ofstream(out / "stress.json") << "{\"version\":2,\"scope\":\"shared-runtime-lifecycle\",\"case\":\"LIFECYCLE\",\"operations\":10,\"cycles\":10,\"rejected\":10,\"frames\":"
        << frames << ",\"valid\":true}\n";
}
}
#endif
