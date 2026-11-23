#include "json_runtime.hpp"
#include "runtime.hpp"
#include "units.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

namespace engine_sim {
struct ClockTestAccess {
    static ClockSource &source(PullEngine &pull) { return pull; }
};

struct RuntimeTestAccess {
    static auto &session(es_runtime &handle) { return *handle.implementation.m_session; }
};
}

namespace {
using namespace engine_sim;
namespace fs = std::filesystem;

constexpr std::size_t Channels = 2;
constexpr std::uint64_t Frequency = 20000;
constexpr auto NativeRate = SampleClock::NativeRate;
using Endpoint = std::array<double, Channels>;
using RuntimeOwner = std::unique_ptr<es_runtime, decltype(&es_destroy)>;

void check(es_status status, const es_error &error) {
    if (status != ES_OK) throw std::runtime_error(error.message);
}

fs::path fixturePath(const char *variable) {
    const auto *path = std::getenv(variable);
    if (path == nullptr) throw std::runtime_error("Missing acoustic test fixture");
    return fs::u8path(path);
}

RuntimeOwner load(std::uint32_t rate) {
    es_error error{};
    es_config config{};
    check(es_config_init(ES_ABI_V1, sizeof(config), &config, &error), error);
    config.output_hz = rate;
    config.seed = 403;
    es_runtime *handle = nullptr;
    check(es_create(&config, &handle, &error), error);
    RuntimeOwner owner(handle, es_destroy);
    check(json_runtime::load_file(handle, fixturePath("ENGINE_SIM_TEST_ACOUSTIC_MODEL"),
                                  fixturePath("ENGINE_SIM_TEST_ACOUSTIC_ASSETS"), nullptr, &error), error);
    return owner;
}

es_telemetry telemetry(const es_runtime &handle) {
    es_telemetry state{};
    state.abi = ES_ABI_V1;
    state.size = sizeof(state);
    es_error error{};
    check(es_get_telemetry(&handle, &state, &error), error);
    return state;
}

std::uint64_t bits(double value) {
    std::uint64_t word;
    static_assert(sizeof word == sizeof value);
    std::memcpy(&word, &value, sizeof word);
    return word;
}

void expectEndpoint(const Endpoint &actual, const Endpoint &expected, std::size_t index) {
    for (std::size_t channel = 0; channel < Channels; ++channel) {
        EXPECT_EQ(bits(actual[channel]), bits(expected[channel])) << "event " << index << ", channel " << channel;
    }
}

// An independent per-cylinder FIFO starts filled with exact zeros. Evaluate
// the endpoint from observed post-fluid pressure and assembled routing.
class PressureOracle {
public:
    explicit PressureOracle(const Engine &engine) {
        EXPECT_EQ(engine.getExhaustSystemCount(), Channels);
        EXPECT_EQ(engine.getCylinderCount(), 4);
        constexpr std::array<int, 4> route{0, 1, 0, 1};
        for (int cylinder = 0; cylinder < engine.getCylinderCount(); ++cylinder) {
            const auto &chamber = *engine.getChamber(cylinder);
            const auto slot = engine.getPiston(cylinder)->getCylinderIndex();
            const auto &head = *engine.getHead(engine.getPiston(cylinder)->getCylinderBank()->getIndex());
            const auto &exhaust = *head.getExhaustSystem(slot);
            const auto length = head.getHeaderPrimaryLength(slot) + exhaust.getLength();
            const auto delay = std::lround(length / (343.0 * units::m / units::sec) * 10000.0);
            if (delay <= 0 || cylinder >= static_cast<int>(route.size()) ||
                exhaust.getIndex() != route[cylinder]) throw std::runtime_error("Unexpected acoustic test graph");
            EXPECT_GT(exhaust.getAudioVolume(), 0.0);
            EXPECT_GT(head.getSoundAttenuation(slot), 0.0);
            m_cylinders.push_back({&chamber, route[cylinder], length, exhaust.getAudioVolume(),
                                   head.getSoundAttenuation(slot), std::vector<double>(delay)});
        }
    }

    Endpoint observe(const Simulator &simulator) {
        Endpoint channels{};
        const auto attenuation = std::min(std::abs(simulator.filteredEngineSpeed()), 40.0) / 40.0;
        for (auto &cylinder : m_cylinders) {
            const auto &pressure = cylinder.chamber->m_exhaustRunnerAndPrimary;
            const auto pulse = (attenuation * attenuation * attenuation) * 1600 *
                (1.0 * (pressure.pressure() - units::pressure(1.0, units::atm))
                 + 0.1 * pressure.dynamicPressure(1.0, 0.0)
                 + 0.1 * pressure.dynamicPressure(-1.0, 0.0));
            const auto delayed = cylinder.history[cylinder.cursor];
            cylinder.history[cylinder.cursor] = pulse;
            cylinder.cursor = (cylinder.cursor + 1) % cylinder.history.size();
            channels[cylinder.channel] += cylinder.attenuation * (cylinder.volume * delayed / m_cylinders.size()) *
                (1 / (cylinder.length * cylinder.length));
        }
        return channels;
    }

private:
    struct Cylinder {
        const CombustionChamber *chamber;
        int channel;
        double length, volume, attenuation;
        std::vector<double> history;
        std::size_t cursor = 0;
    };
    std::vector<Cylinder> m_cylinders;
};

enum class Edge { Tick, Native, Latch };
enum class TickHook { Before, Observer, After };
using Event = std::pair<std::uint64_t, Edge>;

struct BoundaryProbe : ClockSource {
    BoundaryProbe(ClockSource &source, Simulator &simulator) : source(source), simulator(simulator) {}

    void latch() override {
        source.latch();
        events.emplace_back(controls.size() * NativeRate, Edge::Latch);
        const auto &engine = *simulator.getEngine();
        controls.push_back({engine.getSpeedControl(), double(engine.getIgnitionModule()->m_enabled),
                            double(simulator.m_starterMotor.m_enabled)});
    }

    bool tick(double *output, std::size_t capacity) override {
        if (!source.tick(output, capacity)) return false;
        ticks.push_back({output[0], output[1]});
        events.emplace_back(ticks.size() * NativeRate, Edge::Tick);
        return true;
    }

    bool native(AcousticExcitation input, std::int16_t &output) override {
        events.emplace_back(inputs.size() * Frequency, Edge::Native);
        if (input.count != Channels) throw std::runtime_error("Unexpected native channel count");
        inputs.push_back({input.data[0], input.data[1]});
        return source.native(input, output);
    }

    ClockSource &source;
    Simulator &simulator;
    std::vector<Event> events;
    std::vector<Endpoint> ticks, inputs;
    std::vector<std::array<double, 3>> controls;
};

enum class Blocks { Unit, Irregular, Long };
constexpr std::uint32_t Frames = 24011;
struct Command {
    std::uint32_t frame;
    double demand;
    std::uint32_t ignition, starter;
};
constexpr std::array<Command, 6> commands{{
    {0, 0.20, 1, 1}, {1, 0.21, 1, 1}, {147, 0.23, 1, 1},
    {160, 0.25, 1, 1}, {8001, 0.50, 1, 0}, {11003, 0.35, 0, 0},
}};

template<typename Render>
std::vector<float> render(es_runtime &handle, Blocks blocks, Render output) {
    std::vector<float> pcm(Frames);
    constexpr std::array<std::uint32_t, 7> irregular{7, 1, 251, 31, 503, 2, 4099};
    std::size_t block = 0;
    es_error error{};
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const auto command = commands[i];
        const es_controls settings{sizeof(settings), ES_ABI_V1, command.ignition, command.starter,
                                   command.demand, 0.0, 0.0};
        check(es_set_controls(&handle, &settings, &error), error);
        const auto end = i + 1 < commands.size() ? commands[i + 1].frame : Frames;
        for (auto pos = command.frame; pos < end;) {
            auto limit = static_cast<std::uint32_t>(SampleClock::MaximumBlock);
            if (blocks == Blocks::Unit) limit = 1;
            if (blocks == Blocks::Irregular) limit = irregular[block++ % irregular.size()];
            const auto count = std::min(end - pos, limit);
            output(pcm.data() + pos, count, pos + count);
            pos += count;
        }
    }
    return pcm;
}

std::uint64_t tickCount(std::uint64_t frames, std::uint64_t rate) { return frames * Frequency / rate; }
std::uint64_t nativeCount(std::uint64_t frames, std::uint64_t rate) { return (frames * NativeRate + rate - 1) / rate; }

void expectCounts(const ClockCounts &counts, std::uint32_t frames, std::uint32_t rate) {
    EXPECT_EQ(counts.frames, frames);
    EXPECT_EQ(counts.ticks, tickCount(frames, rate));
    EXPECT_EQ(counts.nativeFrames, nativeCount(frames, rate));
}

std::vector<float> publicRender(es_runtime &handle, std::uint32_t rate, Blocks blocks) {
    return render(handle, blocks, [&](float *pcm, std::uint32_t count, std::uint32_t end) {
        es_error error{};
        std::uint32_t written = 0;
        check(es_render(&handle, count, pcm, count, &written, &error), error);
        EXPECT_EQ(written, count);
        const auto state = telemetry(handle);
        EXPECT_EQ(state.simulation_hz, Frequency);
        expectCounts({state.frames, state.ticks, state.native_frames}, end, rate);
    });
}

std::vector<float> observedRender(es_runtime &handle, std::uint32_t rate, Blocks blocks) {
    auto &session = RuntimeTestAccess::session(handle);
    auto &simulator = *session.prepared.session->simulator;
    auto &pull = *session.pull;
    PressureOracle oracle(*simulator.getEngine());
    const auto audio = session.prepared.session->synthesis.getAudioParameters();
    EXPECT_GT(audio.airNoise, 0.0f);
    EXPECT_GT(audio.inputSampleNoise, 0.0f);
    EXPECT_GT(audio.convolution, 0.0f);
    // Observe the real PullEngine source through a fixed-rate test clock.
    // Compare its PCM with the same commands through the public entry point.
    BoundaryProbe source(ClockTestAccess::source(pull), simulator);
    SampleClock clock(Frequency, std::vector<double>(Channels), source, SampleClock::Profile::Public, rate);
    std::vector<TickHook> observations;
    std::vector<Endpoint> expected;
    pull.setTickCommands([&] { observations.push_back(TickHook::Before); },
                         [&] { observations.push_back(TickHook::After); });
    pull.setTickObserver([&](const Simulator &snapshot) {
        observations.push_back(TickHook::Observer);
        expected.push_back(oracle.observe(snapshot));
    });
    auto pcm = render(handle, blocks, [&](float *dst, std::uint32_t count, std::uint32_t end) {
        clock.render(dst, count);
        expectCounts(clock.counts(), end, rate);
    });
    pull.setTickObserver({});
    pull.setTickCommands({}, {});
    EXPECT_EQ(source.ticks.size(), tickCount(Frames, rate));
    EXPECT_EQ(source.inputs.size(), nativeCount(Frames, rate));
    EXPECT_EQ(source.controls.size(), (Frames * Frequency + rate - 1) / rate);
    EXPECT_EQ(observations.size(), 3 * source.ticks.size());
    EXPECT_EQ(expected.size(), source.ticks.size());
    for (std::size_t tick = 0; tick < source.ticks.size(); ++tick) {
        expectEndpoint(source.ticks[tick], expected.at(tick), tick);
        EXPECT_EQ(observations.at(3 * tick), TickHook::Before);
        EXPECT_EQ(observations.at(3 * tick + 1), TickHook::Observer);
        EXPECT_EQ(observations.at(3 * tick + 2), TickHook::After);
    }
    EXPECT_TRUE(std::is_sorted(source.events.begin(), source.events.end()));
    EXPECT_EQ(source.events.front(), (Event{0, Edge::Native}));
    EXPECT_EQ(source.events[1], (Event{0, Edge::Latch}));
    const auto distinct = [](const Endpoint &values) { return values[0] != 0 && values[1] != 0 && values[0] != values[1]; };
    EXPECT_TRUE(std::any_of(source.ticks.begin(), source.ticks.end(), distinct));
    EXPECT_TRUE(std::any_of(source.inputs.begin(), source.inputs.end(), distinct));
    for (std::size_t n = 0; n < source.inputs.size(); ++n) {
        const auto k = n * Frequency / NativeRate;
        const auto frac = static_cast<double>(n * Frequency % NativeRate) / NativeRate;
        const auto previous = k >= 2 ? source.ticks.at(k - 2) : Endpoint{};
        const auto current = k >= 1 ? source.ticks.at(k - 1) : Endpoint{};
        Endpoint interpolated{};
        for (std::size_t channel = 0; channel < Channels; ++channel)
            interpolated[channel] = (1 - frac) * previous[channel] + frac * current[channel];
        expectEndpoint(source.inputs[n], interpolated, n);
    }
    std::size_t next = 1;
    for (std::size_t tick = 0; tick < source.controls.size(); ++tick) {
        while (next < commands.size() && commands[next].frame * Frequency <= tick * rate) ++next;
        const auto command = commands[next - 1];
        const std::array<double, 3> values{command.demand, double(command.ignition), double(command.starter)};
        EXPECT_EQ(source.controls[tick], values) << "latch " << tick;
    }
    return pcm;
}

TEST(AcousticEndpointTest, AcousticEndpointParity) {
    for (const auto rate : {44100u, 48000u}) {
        SCOPED_TRACE(rate);
        auto first = load(rate);
        const auto reference = observedRender(*first, rate, Blocks::Unit);
        for (const auto partition : {Blocks::Irregular, Blocks::Long}) {
            auto handle = load(rate);
            const auto actual = publicRender(*handle, rate, partition);
            EXPECT_EQ(std::memcmp(reference.data(), actual.data(), reference.size() * sizeof(float)), 0);
        }
    }
}

TEST(AcousticEndpointTest, TickBoundaryOrdering) {
    for (const auto rate : {44100u, 48000u}) {
        SCOPED_TRACE(rate);
        auto invalid = load(rate);
        auto &pull = *RuntimeTestAccess::session(*invalid).pull;
        unsigned entered = 0;
        pull.setTickCommands([&] { ++entered; }, [&] { ++entered; });
        pull.setTickObserver([&](const Simulator &) { ++entered; });
        auto &source = ClockTestAccess::source(pull);
        std::array<double, 3> endpoint{1.0, 2.0, 3.0};
        const auto sentinel = endpoint;
        EXPECT_FALSE(source.tick(nullptr, Channels));
        EXPECT_FALSE(source.tick(endpoint.data(), Channels - 1));
        EXPECT_EQ(endpoint, sentinel);
        EXPECT_EQ(entered, 0u);
        pull.setTickCommands({}, {});
        pull.setTickObserver({});

        es_error error{};
        std::uint32_t written = 99;
        float destination = 99.0f;
        EXPECT_EQ(es_render(invalid.get(), 1, nullptr, 1, &written, &error), ES_INVALID);
        EXPECT_EQ(written, 0);
        written = 99;
        EXPECT_EQ(es_render(invalid.get(), 1, &destination, 0, &written, &error), ES_CAPACITY);
        EXPECT_EQ(written, 0);
        EXPECT_EQ(destination, 99.0f);
        const auto state = telemetry(*invalid);
        EXPECT_EQ(state.state, ES_READY);
        expectCounts({state.frames, state.ticks, state.native_frames}, 0, rate);

        auto reference = load(rate);
        const auto observed = observedRender(*reference, rate, Blocks::Irregular);
        const auto publicPcm = publicRender(*invalid, rate, Blocks::Long);
        EXPECT_EQ(std::memcmp(observed.data(), publicPcm.data(), observed.size() * sizeof(float)), 0);

        auto larger = load(rate);
        auto &wide = ClockTestAccess::source(*RuntimeTestAccess::session(*larger).pull);
        wide.latch();
        EXPECT_TRUE(wide.tick(endpoint.data(), endpoint.size()));
        EXPECT_EQ(bits(endpoint[0]), bits(0.0));
        EXPECT_EQ(bits(endpoint[1]), bits(0.0));
        EXPECT_EQ(endpoint[2], sentinel[2]);
    }
}
}
