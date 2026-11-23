#include "legacy_runtime.hpp"
#include "mechanical_observation.hpp"
#include "runtime.hpp"
#include "json_runtime.hpp"
#include "wave_reader.h"
#include <engine_sim/config-json.hpp>
#include "shaft_drive.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <memory>
#include <utility>

namespace engine_sim {
struct RuntimeTestAccess {
    static auto &session(es_runtime &runtime) { return *runtime.implementation.m_session; }
};
}

namespace {
using engine_sim_capture::MechanicalObservation;
using engine_sim::RuntimeTestAccess;
namespace fs = std::filesystem;
namespace cfg = engine_sim::config_v1;
namespace json = engine_sim::json_runtime;

void check(es_status status, const es_error &error) {
    if (status != ES_OK) throw std::runtime_error(error.message);
}

std::uint64_t bits(double value) {
    std::uint64_t result;
    static_assert(sizeof(result) == sizeof(value));
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

std::array<std::uint64_t, 21> hostRecord(const es_telemetry &v) {
    return {v.size, v.abi, v.state, v.mode, v.output_hz, v.simulation_hz, v.cylinders,
            v.reserved, v.frames, v.ticks, v.native_frames,
            bits(v.crank_rad_s), bits(v.measured_rpm), bits(v.cycle_rad),
            bits(v.plate_angle_rad), bits(v.displacement_m3), bits(v.requested_load_nm),
            bits(v.load_reaction_nm), bits(v.drive_reaction_nm),
            bits(v.applied_driven_rpm), bits(v.driven_limit_rpm)};
}

std::array<double, 19> tickFields(const MechanicalObservation &capture, std::size_t i) {
    const auto &a = capture.tick(i);
    const auto &b = capture.cylinder(i, 0);
    return {a.shaftRadPerSecond, a.rpm, a.cycleRad, a.displacementM3,
        a.demand, a.plateRad, a.appliedDriveRpm, a.driveReactionNm, a.loadReactionNm,
        a.mechanismNm, b.pressurePa, b.volumeM3, b.moles, b.intakeMolesPerSecond,
        b.exhaustMolesPerSecond, b.bigEndXNm, b.bigEndYNm, capture.intake(i,0), capture.exhaust(i,0)};
}

std::array<std::uint64_t, 19> tickRecord(const MechanicalObservation &capture, std::size_t index) {
    const auto values = tickFields(capture, index);
    std::array<std::uint64_t, 19> result{};
    std::transform(values.begin(), values.end(), result.begin(), bits);
    return result;
}

// The external runner compares these records to an independent original R2
// witness. The product test checks the same values on live paired sessions.
void exportFixtureTicks(const MechanicalObservation &capture, const std::string &name, std::size_t count = 0) {
    const auto *root = std::getenv("ENGINE_SIM_GATE_OUTPUT");
    if (root == nullptr) return;
    const auto path = fs::u8path(root)/(name+".mechanical.le64");
    if (fs::exists(path)) throw std::runtime_error("mechanical evidence path already exists");
    std::ofstream out(path, std::ios::binary);
    const auto extent = count == 0 ? capture.count() : count;
    for (std::size_t i = 0; i < extent; ++i) {
        for (const auto word : tickRecord(capture, i)) {
            std::array<unsigned char,8> bytes{};
            for (unsigned j = 0; j < bytes.size(); ++j)
                bytes[j] = static_cast<unsigned char>(word >> (j*8));
            out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        }
    }
    out.close();
    if (!out) throw std::runtime_error("mechanical evidence write failed");
}

std::string readFixture(const fs::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    if (size <= 0 || size > 16 * 1024 * 1024) throw std::runtime_error("invalid fixture extent");
    std::string raw(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(raw.data(), static_cast<std::streamsize>(size))) throw std::runtime_error("incomplete fixture");
    return raw;
}

enum class Source { legacy, file, bytes };

class PreparedRuntime {
public:
    PreparedRuntime(const fs::path &assets, const fs::path &library, unsigned rate, std::uint32_t mode,
                    Source source = Source::legacy)
        : m_handle(nullptr, es_destroy), m_source(source) {
        es_error error{};
        es_config config{};
        check(es_config_init(ES_ABI_V2, sizeof(config), &config, &error), error);
        config.mode = mode;
        config.output_hz = rate;
        config.seed = 403;
        es_runtime *handle = nullptr;
        check(es_create(&config, &handle, &error), error);
        m_handle.reset(handle);
        if (source == Source::legacy) {
            m_legacyPaths = {assets/"main.mr", library, assets};
            m_legacyInputs = engine_sim::legacy_runtime::prepare(*handle, m_legacyPaths);
        }
        else if (source == Source::file)
            check(json::load_file(handle, assets/"main.json", assets, nullptr, &error), error);
        else {
            auto input = readFixture(assets/"main.json");
            check(json::load_bytes(handle, input.data(), input.size(), assets, nullptr, &error), error);
            input.assign(input.size(), '!');
        }
    }

    void reset() {
        es_error error{};
        if (m_source == Source::legacy)
            engine_sim::legacy_runtime::prepare(*m_handle, m_legacyPaths, &m_legacyInputs);
        else
            check(json::reset(m_handle.get(), nullptr, &error), error);
        m_observation.reset();
    }

    void observe(std::size_t maxTicks) {
        auto &s = RuntimeTestAccess::session(*m_handle);
        m_observation = std::make_unique<MechanicalObservation>(simulator(), maxTicks);
        if (s.drive) {
            s.pull->setTickCommands([&s] { s.drive->beginTick(); },
                [this, &s] {
                    s.drive->finishTick();
                    m_observation->record(s.drive->loadReactionNm(), s.drive->reactionNm(), s.drive->appliedRpm());
                });
        }
        else {
            s.pull->setTickObserver([this](const Simulator &) {
                m_observation->record(simulator().getExternalLoadReactionNm(), 0, 0);
            });
        }
    }

    es_telemetry current() const {
        es_error error{};
        es_telemetry t{};
        t.size = sizeof(t);
        t.abi = ES_ABI_V2;
        check(es_get_telemetry(m_handle.get(), &t, &error), error);
        return t;
    }

    void set(es_controls value) {
        es_error error{};
        check(es_set_controls(m_handle.get(), &value, &error), error);
    }

    void render(float *output, std::uint32_t count) {
        es_error error{};
        std::uint32_t written = 0;
        check(es_render(m_handle.get(), count, output, count, &written, &error), error);
        if (written != count) throw std::runtime_error("incomplete fixture render");
    }

    float frame() {
        float result = 0;
        render(&result, 1);
        return result;
    }

    const MechanicalObservation &observation() const { return *m_observation; }

private:
    const PistonEngineSimulator &simulator() const {
        return static_cast<const PistonEngineSimulator &>(*RuntimeTestAccess::session(*m_handle).prepared.session->simulator);
    }

    std::unique_ptr<MechanicalObservation> m_observation;
    std::unique_ptr<es_runtime, decltype(&es_destroy)> m_handle;
    Source m_source;
    es_script::PreparationContext m_legacyPaths;
    std::vector<es_script::ScriptInput> m_legacyInputs;
};

class MechanicalObservationTest : public testing::Test {
protected:
    void SetUp() override {
        const auto *source = std::getenv("ENGINE_SIM_TEST_SOURCE");
        const auto *fixtures = std::getenv("ENGINE_SIM_TEST_MECHANICAL_FIXTURE");
        ASSERT_NE(source, nullptr);
        ASSERT_NE(fixtures, nullptr);
        lawful = fs::u8path(source)/"examples/runtime/fixture";
        asymmetric = fs::u8path(fixtures);
    }
    fs::path lawful, asymmetric;
};

TEST_F(MechanicalObservationTest, Isolation) {
    const std::uint64_t count = 24000;
    for (auto rate : {44100u, 48000u}) {
        for (auto mode : {ES_FREE, ES_DRIVEN}) {
            SCOPED_TRACE(rate);
            SCOPED_TRACE(mode);
            PreparedRuntime baseline(lawful, lawful/"library", rate, mode);
            PreparedRuntime candidate(lawful, lawful/"library", rate, mode);
            const auto start = candidate.current();
            const auto expectedTicks = count * start.simulation_hz / rate;
            candidate.observe(expectedTicks);
            EXPECT_EQ(hostRecord(start), hostRecord(baseline.current()));
            const auto &capture = candidate.observation();
            ASSERT_EQ(capture.count(), 1u);
            EXPECT_EQ(capture.seed(), 403u);
            EXPECT_EQ(capture.frequency(), start.simulation_hz);
            ASSERT_EQ(capture.bindings().size(), 1u);
            ASSERT_EQ(capture.bankCurves().size(), 1u);
            const auto &curves = capture.bankCurves().front();
            EXPECT_EQ(curves.intakePort, curves.exhaustPort);
            EXPECT_EQ(curves.intakeCam, curves.exhaustCam);
            EXPECT_NE(capture.ignitionCurve(), curves.intakeCam);
            EXPECT_EQ(capture.curves().at(curves.intakeCam).points.size(), 5u);
            const auto &binding = capture.bindings().front();
            EXPECT_EQ(binding.bank, 0);
            EXPECT_EQ(binding.journal, 0);
            EXPECT_EQ(binding.localCylinder, 0);
            EXPECT_EQ(binding.intake, 0);
            EXPECT_EQ(binding.exhaust, 0);
            EXPECT_TRUE(binding.ignitionAssigned);
            EXPECT_GT(binding.initialVolumeM3, 0);
            for (std::uint64_t i = 0; i < count; ++i) {
                if (i == 0 || i == 8000) {
                    const es_controls next{sizeof(next), ES_ABI_V2, 1, mode == ES_FREE && i == 0,
                        i == 0 ? .2 : .5, 0, mode == ES_DRIVEN ? (i == 0 ? 900. : 1700.) : 0.};
                    baseline.set(next);
                    candidate.set(next);
                }
                const auto oldPcm = baseline.frame();
                const auto observedPcm = candidate.frame();
                ASSERT_EQ(std::memcmp(&oldPcm, &observedPcm, sizeof(oldPcm)), 0) << i;
                ASSERT_EQ(hostRecord(baseline.current()), hostRecord(candidate.current())) << i;
            }
            ASSERT_EQ(capture.count(), expectedTicks + 1);
            EXPECT_EQ(candidate.current().ticks, expectedTicks);
            double endpoint = 0;
            for (std::size_t i = 0; i < capture.count(); ++i) {
                const auto &cylinder = capture.cylinder(i, 0);
                ASSERT_TRUE(std::isfinite(cylinder.pressurePa));
                ASSERT_GT(cylinder.volumeM3, 0);
                ASSERT_GT(cylinder.moles, 0);
                endpoint = std::max(endpoint, std::abs(capture.exhaust(i, 0)));
            }
            EXPECT_GT(endpoint, 0);
            exportFixtureTicks(capture, std::string(mode == ES_FREE ? "free-" : "driven-")+std::to_string(rate));
        }
    }
}

TEST_F(MechanicalObservationTest, ConvertedFixtureStructure) {
    const auto serialized = readFixture(lawful/"main.json");
    const auto model = cfg::read_json(serialized);
    ASSERT_TRUE(std::holds_alternative<cfg::NormalizedDocument>(model));
    const auto &normalized = std::get<cfg::NormalizedDocument>(model);
    EXPECT_EQ(cfg::write_json(normalized), serialized);
    const auto &document = normalized.values();
    const auto &parts = document.components;
    EXPECT_EQ(document.engine.id, document.selected_engine);
    EXPECT_EQ(document.engine.name, "Runtime Single");
    EXPECT_EQ(document.engine.simulation_frequency.value, 10000);
    EXPECT_EQ(normalized.topology().cylinders.size(), 1u);
    ASSERT_EQ(parts.throttles.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<cfg::DirectThrottle>(parts.throttles.front()));
    EXPECT_EQ(std::get<cfg::DirectThrottle>(parts.throttles.front()).gamma.value, 1);
    ASSERT_TRUE(document.vehicle && document.transmission);
    EXPECT_EQ(document.vehicle->mass.value, 1597);
    EXPECT_EQ(document.vehicle->diff_ratio.value, 3.42);
    EXPECT_EQ(document.transmission->max_clutch_torque.value, 1355.817456);
    const std::array<double, 6> gears{2.97, 2.07, 1.43, 1, 0.84, 0.56};
    ASSERT_EQ(document.transmission->gears.size(), gears.size());
    for (std::size_t i = 0; i < gears.size(); ++i)
        EXPECT_EQ(document.transmission->gears[i].value, gears[i]);
    ASSERT_EQ(parts.impulses.size(), 1u);
    EXPECT_EQ(parts.impulses.front().path, "impulse.wav");
    EXPECT_EQ(parts.impulses.front().volume.value, 0.01);
    const std::vector<std::int16_t> samples{32767, 8192, -4096, 2048, -1024, 512, -256, 128};
    EXPECT_EQ(engine_sim::readImpulseWave(lawful/"impulse.wav"), samples);

    PreparedRuntime original(lawful, lawful/"library", 44100, ES_FREE);
    PreparedRuntime candidate(lawful, {}, 44100, ES_FREE, Source::file);
    original.observe(0);
    candidate.observe(0);
    EXPECT_EQ(hostRecord(original.current()), hostRecord(candidate.current()));
    EXPECT_EQ(original.current().frames, 0u);
    const auto &old = original.observation();
    const auto &actual = candidate.observation();
    EXPECT_EQ(actual.seed(), old.seed());
    EXPECT_EQ(actual.frequency(), old.frequency());
    EXPECT_EQ(tickRecord(actual, 0), tickRecord(old, 0));
    ASSERT_EQ(actual.bindings().size(), 1u);
    const auto &binding = actual.bindings().front();
    const auto &old_binding = old.bindings().front();
    EXPECT_EQ(binding.crank, 0);
    EXPECT_EQ(binding.masterRod, -1);
    EXPECT_EQ(binding.journal, 0);
    EXPECT_EQ(binding.bank, 0);
    EXPECT_EQ(binding.localCylinder, 0);
    EXPECT_EQ(binding.intake, 0);
    EXPECT_EQ(binding.exhaust, 0);
    EXPECT_TRUE(binding.ignitionAssigned);
    EXPECT_EQ(bits(binding.ignitionAngleRad), bits(old_binding.ignitionAngleRad));
    EXPECT_EQ(bits(binding.initialVolumeM3), bits(old_binding.initialVolumeM3));
    EXPECT_EQ(bits(binding.pistonXM), bits(old_binding.pistonXM));
    EXPECT_EQ(bits(binding.pistonYM), bits(old_binding.pistonYM));
    EXPECT_EQ(bits(binding.rodAngleRad), bits(old_binding.rodAngleRad));
    ASSERT_EQ(actual.bankCurves().size(), 1u);
    const auto &routes = actual.bankCurves().front();
    EXPECT_EQ(routes.intakeCam, routes.exhaustCam);
    EXPECT_EQ(routes.intakePort, routes.exhaustPort);
    EXPECT_EQ(actual.ignitionCurve(), old.ignitionCurve());
    ASSERT_EQ(actual.curves().size(), old.curves().size());
    for (std::size_t i = 0; i < actual.curves().size(); ++i) {
        const auto &a = actual.curves()[i];
        const auto &b = old.curves()[i];
        EXPECT_EQ(bits(a.radius), bits(b.radius));
        EXPECT_EQ(bits(a.inputScale), bits(b.inputScale));
        EXPECT_EQ(bits(a.outputScale), bits(b.outputScale));
        ASSERT_EQ(a.points.size(), b.points.size());
        for (std::size_t j = 0; j < a.points.size(); ++j) {
            EXPECT_EQ(bits(a.points[j].first), bits(b.points[j].first));
            EXPECT_EQ(bits(a.points[j].second), bits(b.points[j].second));
        }
    }
}

constexpr std::uint32_t ReplayedFrames = 26003;

struct Playback {
    std::vector<float> pcm = std::vector<float>(ReplayedFrames);
    std::vector<std::array<std::uint64_t, 21>> snapshots;
};

Playback play_fixture(PreparedRuntime &runtime, bool driven, const std::vector<std::uint32_t> &partitions) {
    Playback result;
    const std::array<std::uint32_t, 4> edges{0, 8000, 24000, ReplayedFrames};
    std::size_t part = 0;
    for (std::size_t event = 0; event+1 < edges.size(); ++event) {
        const es_controls next{sizeof(next), ES_ABI_V2, 1, !driven && event == 0,
            event == 0 ? .2 : event == 1 ? .5 : .35, event == 2 ? 1.0 : 0.0,
            driven ? (event == 0 ? 900. : event == 1 ? 1700. : 1100.) : 0.};
        runtime.set(next);
        const auto zero = hostRecord(runtime.current());
        runtime.render(nullptr, 0);
        EXPECT_EQ(hostRecord(runtime.current()), zero);
        for (auto position = edges[event]; position < edges[event+1];) {
            const auto length = std::min(partitions[part++ % partitions.size()], edges[event+1] - position);
            runtime.render(result.pcm.data() + position, length);
            position += length;
        }
        result.snapshots.push_back(hostRecord(runtime.current()));
    }
    return result;
}

void expect_playback(const Playback &actual, const Playback &expected) {
    ASSERT_EQ(actual.pcm.size(), expected.pcm.size());
    EXPECT_EQ(std::memcmp(actual.pcm.data(), expected.pcm.data(), actual.pcm.size()*sizeof(float)), 0);
    EXPECT_EQ(actual.snapshots, expected.snapshots);
}

TEST_F(MechanicalObservationTest, ConvertedFixtureParity) {
    for (const auto rate : {44100u, 48000u}) {
        for (const auto mode : {ES_FREE, ES_DRIVEN}) {
            SCOPED_TRACE(rate);
            SCOPED_TRACE(mode);
            const auto driven = mode == ES_DRIVEN;
            const auto ticks = std::size_t{ReplayedFrames} * 10000 / rate;
            PreparedRuntime original(lawful, lawful/"library", rate, mode);
            PreparedRuntime file(lawful, {}, rate, mode, Source::file);
            PreparedRuntime owned(lawful, {}, rate, mode, Source::bytes);
            for (auto *owner : {&original, &file, &owned}) owner->observe(ticks);
            const auto expected = play_fixture(original, driven, {65536});
            const auto actual = play_fixture(file, driven, {1});
            expect_playback(actual, expected);
            expect_playback(play_fixture(owned, driven, {7, 251, 31, 1, 503}), expected);
            ASSERT_TRUE(std::any_of(actual.pcm.begin(), actual.pcm.end(), [](float value) { return value != 0; }));
            ASSERT_EQ(file.observation().count(), ticks+1);
            ASSERT_EQ(owned.observation().count(), ticks+1);
            ASSERT_EQ(original.observation().count(), ticks+1);
            for (std::size_t i = 0; i <= ticks; ++i) {
                const auto observed = tickRecord(file.observation(), i);
                ASSERT_EQ(observed, tickRecord(original.observation(), i)) << i;
                ASSERT_EQ(observed, tickRecord(owned.observation(), i)) << i;
            }
            const auto prefix = 24000u * 10000u / rate + 1;
            exportFixtureTicks(file.observation(),
                std::string(driven ? "converted-driven-" : "converted-free-")+std::to_string(rate), prefix);

            for (auto *owner : {&file, &owned}) {
                owner->reset();
                EXPECT_EQ(owner->current().frames, 0u);
                EXPECT_EQ(owner->current().ticks, 0u);
                expect_playback(play_fixture(*owner, driven, {193, 1, 57}), expected);
            }
        }
    }
}

TEST_F(MechanicalObservationTest, Torque) {
    PreparedRuntime engine(asymmetric, lawful/"library", 44100, ES_DRIVEN);
    engine.observe(10000);
    const auto &capture = engine.observation();
    ASSERT_EQ(capture.bindings().size(), 2u);
    const auto &a = capture.bindings()[0];
    const auto &b = capture.bindings()[1];
    EXPECT_EQ(a.bank, 0);
    EXPECT_EQ(b.bank, 1);
    EXPECT_EQ(a.localCylinder, 0);
    EXPECT_EQ(b.localCylinder, 0);
    EXPECT_EQ(a.intake, b.intake);
    EXPECT_EQ(a.exhaust, 0);
    EXPECT_EQ(b.exhaust, 1);
    EXPECT_EQ(a.crank, 0);
    EXPECT_EQ(b.crank, 0);
    EXPECT_EQ(a.journal, 0);
    EXPECT_EQ(b.journal, 1);
    EXPECT_TRUE(a.ignitionAssigned);
    EXPECT_TRUE(b.ignitionAssigned);
    EXPECT_NE(a.ignitionAngleRad, b.ignitionAngleRad);
    for (unsigned event = 0; event < 85; ++event) {
        const double rpm = std::min(1500., 450. + 15 * event);
        engine.set({sizeof(es_controls), ES_ABI_V2, 1, 0, .45, event < 50 ? 0. : 4., rpm});
        for (unsigned i = 0; i < 441; ++i) engine.frame();
    }
    double min = 0, max = 0, peakDrive = 0;
    bool differentCylinders = false, loaded = false;
    for (std::size_t i = 1; i < capture.count(); ++i) {
        const auto &tick = capture.tick(i);
        const auto &c0 = capture.cylinder(i, 0);
        const auto &c1 = capture.cylinder(i, 1);
        const auto torque0 = c0.bigEndXNm + c0.bigEndYNm;
        const auto torque1 = c1.bigEndXNm + c1.bigEndYNm;
        ASSERT_EQ(bits(tick.mechanismNm), bits(torque0 + torque1));
        ASSERT_LT(tick.shaftRadPerSecond, 0);
        ASSERT_TRUE(std::isfinite(tick.driveReactionNm));
        if (tick.loadReactionNm == 0) {
            min = std::min(min, tick.mechanismNm);
            max = std::max(max, tick.mechanismNm);
        }
        else {
            EXPECT_EQ(tick.loadReactionNm, 4.);
            loaded = true;
        }
        differentCylinders = differentCylinders || torque0 != torque1;
        peakDrive = std::max(peakDrive, std::abs(tick.driveReactionNm));
    }
    EXPECT_LT(min, -1e-3);
    EXPECT_GT(max, 1e-3);
    EXPECT_GT(peakDrive, 1e-3);
    EXPECT_TRUE(loaded);
    EXPECT_TRUE(differentCylinders);
}

// The solver's second row uses -1 for the output body's y translation
// and -r_x for its angle. Recover the angular reaction from a positive y-force.
TEST(MechanicalConventionTest, OneNewtonAtOneMetreIsPositive) {
    atg_scs::RigidBody rod, shaft;
    rod.index = 0;
    shaft.index = 1;
    atg_scs::SystemState state;
    state.resize(2, 2);
    struct Release { atg_scs::SystemState &value; ~Release() { value.destroy(); } } release{state};
    for (int i = 0; i < 2; ++i) {
        state.p_x[i] = i == 0 ? 1. : 0.;
        state.p_y[i] = 0;
        state.theta[i] = 0;
        state.v_theta[i] = 0;
    }
    atg_scs::LinkConstraint link;
    link.setBody1(&rod);
    link.setBody2(&shaft);
    link.setLocalPosition2(1, 0);
    atg_scs::Constraint::Output out{};
    link.calculate(&out, &state);
    ASSERT_EQ(out.C[1], 0);
    ASSERT_EQ(out.J[1][4], -1);
    EXPECT_EQ(out.J[1][5] * (1 / out.J[1][4]), 1);
}
}
