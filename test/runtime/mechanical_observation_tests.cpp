#include "mechanical_observation.hpp"
#include "runtime.hpp"
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

// The external admission runner can compare these fields with its independent
// original R2 witness. The ordinary test still checks the live observations.
void exportFixtureTicks(const MechanicalObservation &capture, const std::string &name) {
    const auto *root = std::getenv("ENGINE_SIM_GATE_OUTPUT");
    if (root == nullptr) return;
    const auto path = fs::u8path(root)/(name+".mechanical.le64");
    if (fs::exists(path)) throw std::runtime_error("mechanical evidence path already exists");
    std::ofstream out(path, std::ios::binary);
    for (std::size_t i = 0; i < capture.count(); ++i) {
        const auto &a = capture.tick(i);
        const auto &b = capture.cylinder(i,0);
        for (double value : {a.shaftRadPerSecond, a.rpm, a.cycleRad, a.displacementM3,
                             a.demand, a.plateRad, a.appliedDriveRpm, a.driveReactionNm,
                             a.loadReactionNm, a.mechanismNm, b.pressurePa, b.volumeM3, b.moles,
                             b.intakeMolesPerSecond, b.exhaustMolesPerSecond, b.bigEndXNm, b.bigEndYNm,
                             capture.intake(i,0), capture.exhaust(i,0)}) {
            const auto word = bits(value);
            std::array<unsigned char,8> bytes{};
            for (unsigned j = 0; j < bytes.size(); ++j)
                bytes[j] = static_cast<unsigned char>(word >> (j*8));
            out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        }
    }
    out.close();
    if (!out) throw std::runtime_error("mechanical evidence write failed");
}

class PreparedRuntime {
public:
    PreparedRuntime(const fs::path &assets, const fs::path &library, unsigned rate, std::uint32_t mode)
        : m_handle(nullptr, es_destroy) {
        es_error error{};
        es_config config{};
        check(es_config_init(ES_ABI_V1, sizeof(config), &config, &error), error);
        config.mode = mode;
        config.output_hz = rate;
        config.seed = 403;
        es_runtime *handle = nullptr;
        check(es_create(&config, &handle, &error), error);
        m_handle.reset(handle);
        check(es_load(handle, (assets/"main.mr").u8string().c_str(), library.u8string().c_str(),
                      assets.u8string().c_str(), &error), error);
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
        t.abi = ES_ABI_V1;
        check(es_get_telemetry(m_handle.get(), &t, &error), error);
        return t;
    }

    void set(es_controls value) {
        es_error error{};
        check(es_set_controls(m_handle.get(), &value, &error), error);
    }

    float frame() {
        es_error error{};
        float result = 0;
        std::uint32_t written = 0;
        check(es_render(m_handle.get(), 1, &result, 1, &written, &error), error);
        if (written != 1) throw std::runtime_error("incomplete mechanical test frame");
        return result;
    }

    const MechanicalObservation &observation() const { return *m_observation; }

private:
    const PistonEngineSimulator &simulator() const {
        return static_cast<const PistonEngineSimulator &>(*RuntimeTestAccess::session(*m_handle).prepared.session->simulator);
    }

    std::unique_ptr<MechanicalObservation> m_observation;
    std::unique_ptr<es_runtime, decltype(&es_destroy)> m_handle;
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
                    const es_controls next{sizeof(next), ES_ABI_V1, 1, mode == ES_FREE && i == 0,
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
        engine.set({sizeof(es_controls), ES_ABI_V1, 1, 0, .45, event < 50 ? 0. : 4., rpm});
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
