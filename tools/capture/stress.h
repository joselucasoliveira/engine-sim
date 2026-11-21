#ifndef ENGINE_SIM_CAPTURE_STRESS_H
#define ENGINE_SIM_CAPTURE_STRESS_H

#include "compiler.h"
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
    std::ofstream(out / "stress.json") << "{\"case\":\"QUEUE\",\"operations\":4000,\"cycles\":1000,\"rejected\":1000,\"valid\":true}\n";
}

inline void stressThread(const std::filesystem::path &out) {
    Synthesizer synth;
    Synthesizer::Parameters p;
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
    std::ofstream(out / "stress.json") << "{\"case\":\"THREAD\",\"operations\":1000,\"cycles\":50,\"frames\":"
        << delivered << ",\"control_updates\":" << updates << ",\"valid\":true}\n";
}

inline void stressLifecycle(const std::filesystem::path &resources, const std::filesystem::path &out,
        const EngineSession::ImpulseReader &reader) {
    es_script::Compiler compiler;
    compiler.initialize({(resources / "es").string(), (resources / "assets").string()});
    std::ofstream inputs(out / "inputs.txt");
    inputs.exceptions(std::ios::badbit | std::ios::failbit);
    const auto invalidEntry = out / "stress-invalid.mr";
    { std::ofstream file(invalidEntry); file << "import \"engine_sim.mr\"\nset_engine(engine())\n"; }
    inputs << invalidEntry.string() << '\n';
    std::unique_ptr<EngineSession> active;
    int frames = 0;
    for (int cycle = 0; cycle < 10; ++cycle) {
        const auto entry = out / ("stress-entry-" + std::to_string(cycle) + ".mr");
        {
            std::ofstream file(entry);
            file << "import \"engine_sim.mr\"\nimport \"engines/"
                << (cycle % 2 ? "atg-video-2/09_radial_9.mr" : "atg-video-1/02_kohler_ch750.mr") << "\"\nmain()\n";
            stressRequire(static_cast<bool>(file), "cannot write lifecycle fixture");
        }
        std::vector<std::string> modules;
        std::ostringstream diagnostics;
        stressRequire(compiler.compile(entry.string(), diagnostics, &modules), "lifecycle compile failed");
        for (const auto &module : modules) inputs << module << '\n';
        auto candidate = EngineSession::prepare(compiler.execute(), [&](const ImpulseResponse &response) {
            inputs << response.getFilename() << '\n';
            return reader(response);
        });
        candidate->simulator->startAudioRenderingThread();
        if (active) active->simulator->endAudioRenderingThread();
        active.swap(candidate);
        stressRequire(active->definition.engine->getCylinderCount() == (cycle % 2 ? 9 : 2), "lifecycle topology mismatch");
        auto &simulator = *active->simulator;
        simulator.m_starterMotor.m_enabled = true;
        active->definition.engine->getIgnitionModule()->m_enabled = true;
        simulator.startFrame(0.01);
        while (simulator.simulateStep()) {}
        simulator.endFrame();
        simulator.synthesizer().waitProcessed();
        std::array<int16_t, 1024> pcm{};
        const auto count = simulator.readAudioOutput(pcm.size(), pcm.data());
        stressRequire(count > 0, "lifecycle produced no PCM");
        frames += count;
        auto *original = active.get();
        stressRequire(compiler.compile(invalidEntry.string()), "negative lifecycle fixture failed to compile");
        bool rejected = false;
        try { auto invalid = EngineSession::prepare(compiler.execute(), reader); active.swap(invalid); }
        catch (const EngineLoadError &) { rejected = true; }
        stressRequire(rejected && active.get() == original, "failed reload replaced the running session");
        std::cout << "completed lifecycle replacement " << cycle + 1 << " of 10\n" << std::flush;
    }
    active.reset();
    std::ofstream(out / "stress.json") << "{\"case\":\"LIFECYCLE\",\"operations\":10,\"cycles\":10,\"rejected\":10,\"frames\":"
        << frames << ",\"valid\":true}\n";
}
}
#endif
