#include "compiler.h"
#include "simulator.h"
#include "synthesizer.h"

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
#include <stdexcept>
#include <string_view>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace engine_sim_capture {
struct FrameAccess {
    static void begin(Simulator &simulator, int ticks) { simulator.beginExactFrame(ticks); }
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
    if (!stream) throw std::runtime_error("cannot write " + path.string());
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream << std::setprecision(17);
    return stream;
}

void writeLittleEndian(std::ostream &stream, std::uint32_t bits, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        stream.put(static_cast<char>((bits >> (8 * i)) & 0xff));
    }
}

std::size_t littleEndian(const std::vector<unsigned char> &data, std::size_t i, int bytes) {
    std::size_t result = 0;
    for (int j = 0; j < bytes; ++j) {
        result |= static_cast<std::size_t>(data[i + j]) << (8 * j);
    }
    return result;
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

bool chunkIs(const std::vector<unsigned char> &data, std::size_t i, std::string_view tag) {
    return std::equal(tag.begin(), tag.end(), data.begin() + i);
}

std::vector<std::int16_t> readImpulse(const fs::path &path) {
    const auto size = fs::file_size(path);
    if (size < 12 || size > 64 * 1024 * 1024) throw std::runtime_error("invalid WAV size: " + path.string());
    std::vector<unsigned char> data(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    stream.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(size));

    const auto invalid = [&]() { return std::runtime_error("invalid PCM16 mono 44100 Hz WAV: " + path.string()); };
    if (!stream || !chunkIs(data, 0, "RIFF") || !chunkIs(data, 8, "WAVE")
        || littleEndian(data, 4, 4) != data.size() - 8) throw invalid();

    bool format = false;
    std::vector<std::int16_t> samples;
    std::size_t offset = 12;
    while (offset + 8 <= data.size()) {
        const auto length = littleEndian(data, offset + 4, 4);
        const auto payload = offset + 8;
        if (length > data.size() - payload) throw invalid();

        if (chunkIs(data, offset, "fmt ")) {
            if (format || length < 16 || littleEndian(data, payload, 2) != 1
                || littleEndian(data, payload + 2, 2) != 1
                || littleEndian(data, payload + 4, 4) != sampleRate
                || littleEndian(data, payload + 8, 4) != sampleRate * 2
                || littleEndian(data, payload + 12, 2) != 2
                || littleEndian(data, payload + 14, 2) != 16) throw invalid();
            format = true;
        }
        else if (chunkIs(data, offset, "data")) {
            if (!samples.empty() || length < 2 || length % 2 != 0) throw invalid();
            samples.reserve(length / 2);
            for (std::size_t i = payload; i < payload + length; i += 2) {
                const int value = static_cast<int>(littleEndian(data, i, 2));
                samples.push_back(static_cast<std::int16_t>(value < 32768 ? value : value - 65536));
            }
        }
        offset = payload + length + length % 2;
    }
    if (offset != data.size() || !format || samples.empty()) throw invalid();
    return samples;
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

void runTone(const fs::path &out, bool quiet) {
    std::uint64_t nonFinite = 0;
    Synthesizer synth;
    Synthesizer::Parameters params;
    params.audioBufferSize = sampleRate;
    params.inputBufferSize = sampleRate;
    if (quiet) {
        params.initialAudioParameters.inputSampleNoise = 0.0f;
        params.initialAudioParameters.airNoise = 0.0f;
    }
    synth.initialize(params);
    engine_sim_capture::FrameAccess::observe(synth, nonFinite);
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

void freeEngine(Engine *engine) {
    engine->destroy();
    delete engine;
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
    return {nullptr, 4, 1, 20000, 1600, false, true};
}

double applySchedule(Engine &engine, Simulator &simulator, const EngineScenario &scenario, int block) {
    const int ignitionOff = scenario.dyno ? 1000 : scenario.steady ? 1400 : 800;
    engine.getIgnitionModule()->m_enabled = block >= 25 && block < ignitionOff;
    simulator.m_starterMotor.m_enabled = block >= 25 && block < 125;
    simulator.m_dyno.m_enabled = simulator.m_dyno.m_hold = scenario.dyno && block >= 500 && block < 800;
    if (simulator.m_dyno.m_hold) {
        const double low = engine.getDynoMinSpeed();
        const double high = engine.getDynoMaxSpeed();
        simulator.m_dyno.m_rotationSpeed = low + (high - low) * (block - 500) / 300.0;
    }
    const double peak = scenario.dyno ? 0.7 : 0.6;
    const int holdEnd = scenario.dyno ? 900 : 700;
    const double control = block < 300 ? 0.1 : block < 500 ? 0.1 + (peak - 0.1) * (block - 300) / 200.0
        : block < holdEnd ? peak : scenario.steady && block < 1300 ? 0.2 : 0.0;
    engine.setSpeedControl(control);
    return control;
}

struct Range {
    double min = std::numeric_limits<double>::infinity();
    double max = -std::numeric_limits<double>::infinity();
    bool valid = true;

    void add(double value) {
        valid = valid && std::isfinite(value);
        min = std::min(min, value);
        max = std::max(max, value);
    }
};

struct CylinderRanges {
    Range pressure, moles, volume, manifold;
};

void runEngine(const fs::path &resources, const fs::path &out, std::string_view name, const fs::path &entryOverride) {
    const auto scenario = selectEngine(name);
    fs::path entry = resources / "assets/main.mr";
    if (!entryOverride.empty()) entry = entryOverride;
    else if (scenario.module != nullptr) {
        entry = out / "entry.mr";
        openOutput(entry) << "import \"engine_sim.mr\"\nimport \"" << scenario.module << "\"\nmain()\n";
    }

    const auto parseStart = Clock::now();
    es_script::Compiler compiler;
    compiler.initialize({(resources / "es").string(), (resources / "assets").string()});
    std::vector<std::string> scriptPaths;
    const bool compiled = compiler.compile(entry.string(), std::cerr, &scriptPaths);
    auto parsed = compiled ? compiler.execute() : es_script::Compiler::Output{};
    compiler.destroy();
    const auto parseTime = microsecondsSince(parseStart);
    if (!compiled) throw std::runtime_error("script compile failed: " + entry.string());

    std::unique_ptr<Engine, decltype(&freeEngine)> engine(parsed.engine, freeEngine);
    std::unique_ptr<Vehicle> vehicle(parsed.vehicle);
    std::unique_ptr<Transmission> transmission(parsed.transmission);
    if (!engine || !vehicle || !transmission || engine->getCylinderCount() != scenario.cylinders
        || engine->getExhaustSystemCount() != scenario.exhausts
        || engine->getSimulationFrequency() != scenario.frequency) {
        throw std::runtime_error("unexpected or unsupported engine graph: " + entry.string());
    }

    auto inputs = openOutput(out / "inputs.txt");
    for (const auto &script : scriptPaths) inputs << script << '\n';
    struct Impulse {
        std::vector<std::int16_t> samples;
        float volume;
    };
    std::vector<Impulse> impulses;
    for (int i = 0; i < scenario.exhausts; ++i) {
        auto *impulse = engine->getExhaustSystem(i)->getImpulseResponse();
        if (impulse == nullptr) throw std::runtime_error("missing impulse response on exhaust " + std::to_string(i));
        const fs::path file = impulse->getFilename();
        inputs << file.string() << '\n';
        impulses.push_back({readImpulse(file), static_cast<float>(impulse->getVolume())});
    }

    engine->calculateDisplacement();
    auto graph = openOutput(out / "graph.tsv");
    graph << engine->getName() << '\t' << engine->getCylinderCount() << '\t'
          << engine->getExhaustSystemCount() << '\t' << engine->getSimulationFrequency() << '\t'
          << engine->getDisplacement() << '\n';
    if (name == "LOAD") return;
    const auto initStart = Clock::now();
    const auto release = [](Simulator *pointer) { pointer->releaseSimulation(); delete pointer; };
    std::unique_ptr<Simulator, decltype(release)> simulator(engine->createSimulator(vehicle.get(), transmission.get()), release);
    simulator->setSimulationFrequency(scenario.frequency);
    auto &synth = simulator->synthesizer();
    auto audio = synth.getAudioParameters();
    audio.inputSampleNoise = engine->getInitialJitter();
    audio.airNoise = engine->getInitialNoise();
    audio.dF_F_mix = engine->getInitialHighFrequencyGain();
    synth.setAudioParameters(audio);
    for (int i = 0; i < scenario.exhausts; ++i) {
        synth.initializeImpulseResponse(impulses[i].samples.data(),
            static_cast<unsigned int>(impulses[i].samples.size()), impulses[i].volume, i);
    }

    std::uint64_t nonFinite = 0;
    engine_sim_capture::FrameAccess::observe(synth, nonFinite);
    synth.setInputSampleRate(scenario.frequency);
    synth.endInputBlock();
    auto clocks = openOutput(out / "startup.tsv");
    clocks << parseTime << '\t' << microsecondsSince(initStart) << '\n';
    auto pcm = openOutput(out / "audio.s16le");
    auto trace = openOutput(out / "trace.csv");
    trace << "block,ticks,sum_ticks,sim_s,input_control,starter,ignition,rpm,rad_s,dyno_nm,manifold_pa,chamber0_pa,chamber0_mol,chamber0_m3,pcm_frames,"
          << "simulation_us,synthesis_us,throttle,dyno_enabled,dyno_target_rad_s,input_phase,pressure_min,pressure_max,"
          << "moles_min,moles_max,volume_min,volume_max,manifold_min,manifold_max,finite,input_queue_frames\n";
    int totalTicks = 0;
    int totalFrames = 0;
    const int ticksPerBlock = scenario.frequency / 100;
    for (int block = 0; block < scenario.blocks; ++block) {
        const double control = applySchedule(*engine, *simulator, scenario, block);
        const auto simulationStart = Clock::now();
        engine_sim_capture::FrameAccess::begin(*simulator, ticksPerBlock);
        CylinderRanges ranges;
        int ticks = 0;
        while (simulator->simulateStep()) {
            ++ticks;
            ranges.manifold.add(engine->getManifoldPressure());
            for (int cylinder = 0; cylinder < scenario.cylinders; ++cylinder) {
                const auto &gas = engine->getChamber(cylinder)->m_system;
                ranges.pressure.add(gas.pressure());
                ranges.moles.add(gas.n());
                ranges.volume.add(gas.volume());
            }
        }
        if (ticks != ticksPerBlock) throw std::runtime_error("mechanical tick count differs from schedule");
        simulator->endFrame();
        const double simulationTime = microsecondsSince(simulationStart);
        const auto synthesisStart = Clock::now();
        size_t inputWindow = 0;
        const int frames = renderAndWrite(synth, pcm, inputWindow);
        const double synthesisTime = microsecondsSince(synthesisStart);
        totalTicks += ticks;
        totalFrames += frames;

        const auto &gas = engine->getChamber(0)->m_system;
        const double rpm = engine->getRpm();
        const double signedSpeed = engine->getCrankshaft(0)->m_body.v_theta;
        const double torque = simulator->getFilteredDynoTorque();
        const bool valid = std::isfinite(rpm) && std::isfinite(signedSpeed) && std::isfinite(torque)
            && ranges.pressure.valid && ranges.moles.valid && ranges.volume.valid && ranges.manifold.valid
            && ranges.pressure.min > 0 && ranges.moles.min >= 0 && ranges.volume.min > 0 && ranges.manifold.min > 0;
        trace << block << ',' << ticks << ',' << totalTicks << ',' << double(totalTicks) / scenario.frequency
              << ',' << control << ',' << simulator->m_starterMotor.m_enabled << ',' << engine->getIgnitionModule()->m_enabled
              << ',' << rpm << ',' << signedSpeed << ',' << torque << ',' << engine->getManifoldPressure()
              << ',' << gas.pressure() << ',' << gas.n() << ',' << gas.volume() << ',' << frames
              << ',' << simulationTime << ',' << synthesisTime << ',' << engine->getThrottle()
              << ',' << simulator->m_dyno.m_enabled << ',' << simulator->m_dyno.m_rotationSpeed << ',' << synth.m_inputWriteOffset
              << ',' << ranges.pressure.min << ',' << ranges.pressure.max << ',' << ranges.moles.min << ',' << ranges.moles.max
              << ',' << ranges.volume.min << ',' << ranges.volume.max << ',' << ranges.manifold.min << ',' << ranges.manifold.max
              << ',' << valid << ',' << inputWindow << '\n';
        if (!valid) throw std::runtime_error("nonfinite or nonphysical state in block " + std::to_string(block));
    }
    openOutput(out / "quantizer.txt") << nonFinite << '\n';
    if (nonFinite != 0 || totalFrames <= 0) throw std::runtime_error("prequantizer was nonfinite or PCM was empty");
}
}

int captureWorker(int argc, char **argv) {
    try {
        if (argc < 6 || argc > 7) throw std::runtime_error("invalid worker arguments");
        const std::string_view name = argv[2];
        const fs::path resources = argv[3];
        const fs::path out = argv[4];
        std::srand(static_cast<unsigned int>(std::stoul(argv[5])));
        if (name == "FIR") runFir(out);
        else if (name == "GAS") runGas(out);
        else if (name == "TONE" || name == "TONE-QUIET") runTone(out, name == "TONE-QUIET");
        else if (name == "KOHLER" || name == "LS" || name == "EJ25" || name == "LOAD") {
            runEngine(resources, out, name, argc == 7 ? fs::path(argv[6]) : fs::path());
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
