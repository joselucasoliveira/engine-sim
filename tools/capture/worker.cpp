#include "../../src/runtime/capture_runtime.hpp"
#include "simulator.h"
#include "synthesizer.h"
#include "stress.h"
#include "input_evidence.h"
#include "wave_reader.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace engine_sim_capture {
struct PrimitiveAccess {
    static void observe(Synthesizer &synth, std::uint64_t &counter) {
        synth.m_nonFiniteCaptureCount = &counter;
    }
};
}

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr int sampleRate = 44100;

double microsecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

std::ofstream openOutput(const fs::path &path) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot write " + path.u8string());
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream << std::setprecision(17);
    return stream;
}

void writeLittleEndian(std::ostream &stream, std::uint32_t bits, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        stream.put(static_cast<char>((bits >> (8 * i)) & 0xff));
    }
}

void recordProcessUsage(const fs::path &directory) {
#if defined(__unix__) || defined(__APPLE__)
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        const auto seconds = [](const timeval &time) { return time.tv_sec + time.tv_usec * 1e-6; };
#if defined(__APPLE__)
        const auto residentBytes = usage.ru_maxrss;
#else
        const auto residentBytes = usage.ru_maxrss * 1024;
#endif
        openOutput(directory / "process.tsv") << seconds(usage.ru_utime) << '\t'
            << seconds(usage.ru_stime) << '\t' << residentBytes << '\n';
    }
#else
    (void)directory;
#endif
}

void runFir(const fs::path &out) {
    constexpr std::array<float, 4> taps{0.5f, -0.25f, 0.125f, 0.0625f};
    ConvolutionFilter filter;
    filter.initialize(static_cast<int>(taps.size()));
    std::copy(taps.begin(), taps.end(), filter.getImpulseResponse());
    auto signal = openOutput(out / "audio.f32le");
    bool valid = true;
    for (int i = 0; i < sampleRate; ++i) {
        const auto phase = static_cast<std::size_t>(i % 257);
        const float sample = filter.f(phase == 0 ? 1.0f : 0.0f);
        valid = valid && sample == (phase < taps.size() ? taps[phase] : 0.0f);
        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(sample));
        std::memcpy(&bits, &sample, sizeof(bits));
        writeLittleEndian(signal, bits, 4);
    }
    filter.destroy();
    if (!valid) throw std::runtime_error("FIR convolution differs from the analytic pulse response");
}

void runGas(const fs::path &out) {
    auto trace = openOutput(out / "trace.csv");
    trace << "step,volume_m3,p_pa,t_k,n_mol,energy_j,work_j\n";
    GasSystem vessel;
    const double volume = units::volume(1.0, units::cc);
    vessel.initialize(units::atm, volume, units::celcius(25.0));
    const auto moles = vessel.n();
    const auto initialEnergy = vessel.kineticEnergy();
    double work = 0.0;
    for (int step = 0; step <= 10000; ++step) {
        if (step > 0) {
            const auto nextVolume = volume * (1.0 - 0.5 * step / 10000.0);
            work += vessel.pressure() * (vessel.volume() - nextVolume);
            vessel.setVolume(nextVolume);
        }
        const double expectedPressure = moles * constants::R * vessel.temperature() / vessel.volume();
        if (!(vessel.n() == moles && vessel.volume() > 0 && vessel.temperature() > 0
            && std::isfinite(vessel.pressure()) && vessel.pressure() > 0
            && std::abs(vessel.pressure() - expectedPressure) < 1e-6)) {
            throw std::runtime_error("closed gas vessel state or ideal-gas invariant failed at " + std::to_string(step));
        }
        if (step % 100 == 0) {
            trace << step << ',' << vessel.volume() << ',' << vessel.pressure() << ',' << vessel.temperature()
                  << ',' << vessel.n() << ',' << vessel.kineticEnergy() << ',' << work << '\n';
        }
    }
    if (std::abs(vessel.kineticEnergy() - initialEnergy - work) > 1e-4) {
        throw std::runtime_error("closed gas work and energy differ");
    }
}

int renderAndWrite(Synthesizer &synth, std::ostream &output, size_t &inputWindow) {
    // The trace retains the published input window before acknowledgement.
    // Producer staging may already be empty after handing off that window.
    inputWindow = synth.getBufferStatistics().pendingInput;
    synth.renderAudio();
    std::array<std::int16_t, 4096> buffer{};
    const int available = static_cast<int>(synth.getBufferStatistics().queuedOutput);
    if (available > static_cast<int>(buffer.size())) throw std::runtime_error("capture buffer is too small");
    const int frames = synth.readAudioOutput(available, buffer.data());
    if (frames != available) throw std::runtime_error("audio drain count differs from queue count");
    for (int i = 0; i < frames; ++i) {
        writeLittleEndian(output, static_cast<std::uint16_t>(buffer[i]), 2);
    }
    return frames;
}

void runTone(const fs::path &out, bool quiet, std::uint64_t seed) {
    std::uint64_t nonFinite = 0;
    Synthesizer synth;
    Synthesizer::Parameters params;
    params.seed = seed;
    params.audioBufferSize = sampleRate;
    params.inputBufferSize = sampleRate;
    if (quiet) {
        params.initialAudioParameters.inputSampleNoise = 0.0f;
        params.initialAudioParameters.airNoise = 0.0f;
    }
    synth.initialize(params);
    engine_sim_capture::PrimitiveAccess::observe(synth, nonFinite);
    constexpr std::int16_t taps[]{32767, -8192, 2048};
    synth.initializeImpulseResponse(taps, 3, 1.0f, 0);
    auto pcm = openOutput(out / "audio.s16le");
    auto trace = openOutput(out / "trace.csv");
    trace << "block,ticks,pcm_frames,input_phase,input_queue_frames,synthesis_us\n";
    int total = 0;
    for (int block = 0; block < 100; ++block) {
        for (int tick = 0; tick < 100; ++tick) {
            const double signal = 1000.0 * std::sin((block * 100 + tick) * 0.03125);
            synth.writeInput(&signal);
        }
        synth.endInputBlock();
        const auto start = Clock::now();
        size_t inputWindow = 0;
        const int frames = renderAndWrite(synth, pcm, inputWindow);
        total += frames;
        trace << block << ",100," << frames << ',' << synth.m_inputWriteOffset << ','
              << inputWindow << ',' << microsecondsSince(start) << '\n';
    }
    synth.destroy();
    openOutput(out / "quantizer.txt") << nonFinite << '\n';
    if (nonFinite != 0 || total <= 0) throw std::runtime_error("tone contains nonfinite data or no frames");
}

struct EngineScenario {
    const char *module;
    int cylinders;
    int exhausts;
    int frequency;
    int blocks;
    bool dyno;
    bool steady;
};

EngineScenario selectEngine(std::string_view name) {
    if (name == "KOHLER") return {"engines/atg-video-1/02_kohler_ch750.mr", 2, 1, 30000, 1000, false, false};
    if (name == "LS") return {"engines/atg-video-2/07_gm_ls.mr", 8, 2, 10000, 1200, true, false};
    if (name == "HONDA") return {"engines/atg-video-1/05_honda_vtec.mr", 4, 2, 20000, 1600, false, true};
    return {nullptr, 4, 1, 20000, 1600, false, true};
}

engine_sim_capture::Controls schedule(const Engine &engine, const EngineScenario &scenario, int block, bool mechanics) {
    engine_sim_capture::Controls controls;
    controls.starter = block >= 25 && block < 125;
    if (mechanics) {
        controls.ignition = block >= 25 && block < 300;
        controls.demand = block < 150 ? 0.10 : block < 300 ? 0.35 : 0.0;
        return controls;
    }
    const int ignitionOff = scenario.dyno ? 1000 : scenario.steady ? 1400 : 800;
    controls.ignition = block >= 25 && block < ignitionOff;
    controls.dyno = scenario.dyno && block >= 500 && block < 800;
    controls.dynoSpeed = engine.getDynoMinSpeed() + (engine.getDynoMaxSpeed() - engine.getDynoMinSpeed()) * (block - 500) / 300.0;
    if (!controls.dyno) controls.dynoSpeed = 0;
    const double peak = scenario.dyno ? 0.7 : 0.6;
    const int holdEnd = scenario.dyno ? 900 : 700;
    controls.demand = block < 300 ? 0.1 : block < 500 ? 0.1 + (peak - 0.1) * (block - 300) / 200.0
        : block < holdEnd ? peak : scenario.steady && block < 1300 ? 0.2 : 0.0;
    return controls;
}

struct MechanicsTrace {
    explicit MechanicsTrace(const fs::path &out)
        : shaft(openOutput(out / "mechanics-v1.csv")),
          combustion(openOutput(out / "combustion-v1.csv")),
          queues(openOutput(out / "queues-v1.csv")) {
        shaft << "tick,cycle_rad,crank_rad_s,demand,plate_rad,starter,ignition\n";
        combustion << "tick,cylinder\n";
        queues << "tick,block_frames,rendered,delivered,pending_input,queued_output,"
                  "dropped_input,dropped_output,zero_filled\n";
    }

    void tick(const engine_sim_capture::TickSample &sample, int cylinders) {
        shaft << sample.tick << ',' << sample.cycle << ',' << sample.speed << ',' << sample.demand << ',' << sample.plate
              << ',' << sample.starter << ',' << sample.ignition << '\n';
        for (int cylinder = 0; cylinder < cylinders; ++cylinder) {
            if (sample.lit & (std::uint64_t{1} << cylinder)) combustion << sample.tick << ',' << cylinder << '\n';
        }
    }

    void output(const es_telemetry &counts, int frames, const Synthesizer::BufferStatistics &stats) {
        queues << counts.ticks << ',' << frames << ',' << counts.native_frames << ',' << counts.frames
               << ',' << stats.pendingInput << ',' << stats.queuedOutput << ',' << stats.droppedInput
               << ',' << stats.droppedOutput << ',' << stats.zeroFilledOutput << '\n';
    }

    std::ofstream shaft, combustion, queues;
};

void runEngine(const fs::path &resources, const fs::path &out, std::string_view name, const fs::path &entryOverride,
               bool captureMechanics, std::uint64_t seed) {
    auto scenario = selectEngine(name);
    if (captureMechanics) scenario.blocks = 400;
    fs::path entry = resources / "assets/main.mr";
    if (!entryOverride.empty()) entry = entryOverride;
    else if (scenario.module != nullptr) {
        entry = out / "entry.mr";
        openOutput(entry) << "import \"engine_sim.mr\"\nimport \"" << scenario.module << "\"\nmain()\n";
    }

    const auto parseStart = Clock::now();
    auto runtime = engine_sim_capture::RuntimeHost::prepare(
        {entry, resources / "es", resources / "assets"}, seed, captureMechanics);
    const auto prepareTime = microsecondsSince(parseStart);
    const auto &engine = runtime->engine();
    const auto &simulator = runtime->simulator();
    if (engine.getCylinderCount() != scenario.cylinders || engine.getExhaustSystemCount() != scenario.exhausts
        || simulator.getSimulationFrequency() != scenario.frequency)
        throw std::runtime_error("unexpected or unsupported engine graph: " + entry.u8string());
    engine_sim_capture::InputEvidence inputs(out);
    inputs.append(runtime->inputs());
    auto graph = openOutput(out / "graph.tsv");
    graph << engine.getName() << '\t' << engine.getCylinderCount() << '\t'
          << engine.getExhaustSystemCount() << '\t' << engine.getSimulationFrequency() << '\t'
          << engine.getDisplacement() << '\n';
    openOutput(out / "capture-v2.json") << "{\"version\":2,\"scope\":\"shared-runtime-engine\","
        "\"clock\":\"half-open-44100\",\"block_frames\":441,\"legacy_endpoint_frames\":1,"
        "\"startup_columns\":[\"prepare_us\",\"reserved_zero\"],"
        "\"timing_columns\":[\"combined_runtime_render_us\",\"reserved_zero\"]}\n";
    if (name == "LOAD") return;
    openOutput(out / "startup.tsv") << prepareTime << "\t0\n";
    auto pcm = openOutput(out / "audio.s16le");
    auto trace = openOutput(out / "trace.csv");
    trace << "block,ticks,sum_ticks,sim_s,input_control,starter,ignition,rpm,rad_s,dyno_nm,manifold_pa,chamber0_pa,chamber0_mol,chamber0_m3,pcm_frames,"
          << "simulation_us,synthesis_us,throttle,dyno_enabled,dyno_target_rad_s,input_phase,pressure_min,pressure_max,"
          << "moles_min,moles_max,volume_min,volume_max,manifold_min,manifold_max,finite,input_queue_frames\n";
    std::optional<MechanicsTrace> mechanics;
    if (captureMechanics) {
        mechanics.emplace(out);
        mechanics->tick(runtime->current(), scenario.cylinders);
    }
    const int ticksPerBlock = scenario.frequency / 100;
    for (int block = 0; block < scenario.blocks; ++block) {
        const auto controls = schedule(engine, scenario, block, captureMechanics);
        runtime->setControls(controls);
        runtime->beginBlock();
        std::array<float, 441> buffer{};
        std::uint32_t written = 0;
        const auto renderStart = Clock::now();
        runtime->render(buffer.data(), buffer.size(), written);
        const double renderTime = microsecondsSince(renderStart);
        runtime->endBlock();
        const auto &counts = runtime->telemetry();
        const auto &ranges = runtime->diagnostics();
        const auto stats = simulator.synthesizer().getBufferStatistics();
        if (stats.stagedInput || stats.pendingInput || stats.activeInput || stats.queuedOutput
            || stats.droppedInput || stats.droppedOutput || stats.zeroFilledOutput)
            throw std::runtime_error("runtime capture unexpectedly used or lost legacy queued audio");
        if (written != buffer.size() || ranges.ticks != static_cast<std::size_t>(ticksPerBlock)
            || counts.frames != std::uint64_t(block + 1) * 441 || counts.native_frames != counts.frames
            || counts.ticks != std::uint64_t(block + 1) * ticksPerBlock)
            throw std::runtime_error("runtime counts differ from the half-open schedule");
        for (float value : buffer) {
            if (!std::isfinite(value) || value < -1 || value > 32767.0f / 32768)
                throw std::runtime_error("nonfinite or unnormalized runtime sample");
            writeLittleEndian(pcm, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(value * 32768))), 2);
        }
        if (mechanics) {
            for (std::size_t i = 0; i < ranges.ticks; ++i) mechanics->tick(ranges.samples[i], scenario.cylinders);
            mechanics->output(counts, written, stats);
        }
        const auto &gas = engine.getChamber(0)->m_system;
        const double rpm = engine.getRpm(), signedSpeed = engine.getCrankshaft(0)->m_body.v_theta;
        const double torque = simulator.getFilteredDynoTorque();
        const bool valid = std::isfinite(rpm) && std::isfinite(signedSpeed) && std::isfinite(torque)
            && ranges.pressure.valid && ranges.moles.valid && ranges.volume.valid && ranges.manifold.valid
            && ranges.pressure.min > 0 && ranges.moles.min >= 0 && ranges.volume.min > 0 && ranges.manifold.min > 0;
        trace << block << ',' << ranges.ticks << ',' << counts.ticks << ',' << double(counts.frames) / sampleRate
              << ',' << controls.demand << ',' << simulator.m_starterMotor.m_enabled << ',' << engine.getIgnitionModule()->m_enabled
              << ',' << rpm << ',' << signedSpeed << ',' << torque << ',' << engine.getManifoldPressure()
              << ',' << gas.pressure() << ',' << gas.n() << ',' << gas.volume() << ',' << written
              << ',' << renderTime << ",0," << engine.getThrottle()
              << ',' << simulator.m_dyno.m_enabled << ',' << simulator.m_dyno.m_rotationSpeed << ",0,"
              << ranges.pressure.min << ',' << ranges.pressure.max << ',' << ranges.moles.min << ',' << ranges.moles.max
              << ',' << ranges.volume.min << ',' << ranges.volume.max << ',' << ranges.manifold.min << ',' << ranges.manifold.max
              << ',' << valid << ',' << stats.pendingInput << '\n';
        if (!valid) throw std::runtime_error("nonfinite or nonphysical state in block " + std::to_string(block));
    }
    openOutput(out / "quantizer.txt") << "0\n";
}
}

int captureWorker(int argc, char **argv) {
    try {
        if (argc < 6 || argc > 7) throw std::runtime_error("invalid worker arguments");
        const std::string_view name = argv[2];
        const fs::path resources = fs::u8path(argv[3]);
        const fs::path out = fs::u8path(argv[4]);
        const auto seed = std::stoull(argv[5]);
        if (name == "FIR") runFir(out);
        else if (name == "GAS") runGas(out);
        else if (name == "TONE" || name == "TONE-QUIET") runTone(out, name == "TONE-QUIET", seed);
        else if (name == "QUEUE") engine_sim_capture::stressQueue(out);
        else if (name == "THREAD") engine_sim_capture::stressThread(out, seed);
        else if (name == "LIFECYCLE") engine_sim_capture::stressLifecycle(resources, out, seed);
        else if (name == "KOHLER" || name == "LS" || name == "EJ25" || name == "HONDA" || name == "LOAD") {
            runEngine(resources, out, name, argc == 7 ? fs::u8path(argv[6]) : fs::path(), false, seed);
        }
        else if (name == "MECHANICS-KOHLER" || name == "MECHANICS-LS") {
            runEngine(resources, out, name.substr(std::string_view("MECHANICS-").size()), {}, true, seed);
        }
        else throw std::runtime_error("unsupported capture workload " + std::string(name));
        recordProcessUsage(out);
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 3;
    }
}
