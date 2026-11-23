#ifndef ENGINE_SIM_CAPTURE_MECHANICAL_OBSERVATION_HPP
#define ENGINE_SIM_CAPTURE_MECHANICAL_OBSERVATION_HPP

#include "piston_engine_simulator.h"
#include "constants.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace engine_sim_capture {
struct CylinderBinding {
    int bank, localCylinder, intake, exhaust, crank, masterRod, journal;
    double ignitionAngleRad, initialVolumeM3;
    double pistonXM, pistonYM, pistonAngleRad, rodXM, rodYM, rodAngleRad;
    bool ignitionAssigned;
};

struct CurveDescriptor {
    double radius, inputScale, outputScale;
    std::vector<std::pair<double, double>> points;
};

struct BankCurves {
    std::size_t intakePort, exhaustPort, intakeCam, exhaustCam;
};

struct CylinderTick {
    double pressurePa, volumeM3, moles, intakeMolesPerSecond, exhaustMolesPerSecond;
    double bigEndXNm, bigEndYNm;
};

struct MechanicalTick {
    double shaftRadPerSecond, rpm, cycleRad, demand, plateRad, displacementM3;
    double mechanismNm, loadReactionNm, driveReactionNm, appliedDriveRpm;
};

class MechanicalObservation {
    // These member pointers apply to the actual prepared base objects. The
    // diagnostic never constructs a derived simulator or changes a solver row.
    struct SimulatorAccess : PistonEngineSimulator {
        static auto rods() { return &SimulatorAccess::m_linkConstraints; }
        static auto shafts() { return &SimulatorAccess::m_crankshaftLinks; }
        static auto endpoints() { return &SimulatorAccess::m_exhaustFlowStagingBuffer; }
    };
    struct IgnitionAccess : IgnitionModule {
        static auto plugs() { return &IgnitionAccess::m_plugs; }
        static auto timing() { return &IgnitionAccess::m_timingCurve; }
    };
    struct HeadAccess : CylinderHead {
        static auto intake() { return &HeadAccess::m_intakePortFlow; }
        static auto exhaust() { return &HeadAccess::m_exhaustPortFlow; }
    };
    struct CurveAccess : Function {
        static auto size() { return &CurveAccess::m_size; }
        static auto x() { return &CurveAccess::m_x; }
        static auto y() { return &CurveAccess::m_y; }
        static auto radius() { return &CurveAccess::m_filterRadius; }
        static auto inputScale() { return &CurveAccess::m_inputScale; }
        static auto outputScale() { return &CurveAccess::m_outputScale; }
    };

public:
    MechanicalObservation(const PistonEngineSimulator &simulator, std::size_t maximumTicks)
        : m_simulator(simulator), m_engine(*simulator.getEngine()) {
        const int cylinders = m_engine.getCylinderCount();
        const int shafts = m_engine.getCrankshaftCount();
        const int exhausts = m_engine.getExhaustSystemCount();
        const int intakes = m_engine.getIntakeCount();
        if (maximumTicks > 100000 || cylinders < 1 || cylinders > 64 || shafts < 1 || shafts > 64 ||
            exhausts < 1 || exhausts > 64 || intakes < 1 || intakes > 64)
            throw std::invalid_argument("mechanical observation exceeds its supported extent");
        m_cylinders = static_cast<std::size_t>(cylinders);
        m_couplings = static_cast<std::size_t>(shafts - 1);
        m_exhausts = static_cast<std::size_t>(exhausts);
        m_intakes = static_cast<std::size_t>(intakes);
        const auto wordsPerTick = 10 + 7*m_cylinders + m_couplings + m_exhausts + m_intakes;
        if ((maximumTicks + 1) * wordsPerTick > 12'000'000)
            throw std::invalid_argument("mechanical observation storage bound exceeded");
        m_rows.resize(maximumTicks + 1);
        m_cylinderRows.resize(m_rows.size() * m_cylinders);
        m_couplingRows.resize(m_rows.size() * m_couplings);
        m_exhaustRows.resize(m_rows.size() * m_exhausts);
        m_intakeRows.resize(m_rows.size() * m_intakes);
        for (int cylinder = 0; cylinder < cylinders; ++cylinder) {
            const auto &piston = *m_engine.getPiston(cylinder);
            const auto &rod = *piston.getRod();
            const auto bank = piston.getCylinderBank()->getIndex();
            const auto local = piston.getCylinderIndex();
            const auto &head = *m_engine.getHead(bank);
            const auto *plugs = (*m_engine.getIgnitionModule()).*IgnitionAccess::plugs();
            m_bindings.push_back({bank, local,
                componentIndex(head.getIntake(local), intakes, [&](int i) { return m_engine.getIntake(i); }),
                head.getExhaustSystem(local)->getIndex(),
                componentIndex(rod.getCrankshaft(), shafts, [&](int i) { return m_engine.getCrankshaft(i); }),
                componentIndex(rod.getMasterRod(), cylinders, [&](int i) { return m_engine.getConnectingRod(i); }),
                rod.getJournal(), plugs[cylinder].angle,
                m_engine.getChamber(cylinder)->m_system.volume() / units::m3,
                piston.m_body.p_x, piston.m_body.p_y, piston.m_body.theta,
                rod.m_body.p_x, rod.m_body.p_y, rod.m_body.theta, plugs[cylinder].enabled});
        }
        std::vector<const Function *> identities;
        m_ignitionCurve = observeCurve((*m_engine.getIgnitionModule()).*IgnitionAccess::timing(), identities);
        for (int bank = 0; bank < m_engine.getCylinderBankCount(); ++bank) {
            const auto &head = *m_engine.getHead(bank);
            m_bankCurves.push_back({
                observeCurve(head.*HeadAccess::intake(), identities),
                observeCurve(head.*HeadAccess::exhaust(), identities),
                observeCurve(head.getIntakeCamshaft()->getLobeProfile(), identities),
                observeCurve(head.getExhaustCamshaft()->getLobeProfile(), identities)});
        }
        record(0, 0, 0);
    }

    MechanicalObservation(const MechanicalObservation &) = delete;
    MechanicalObservation &operator=(const MechanicalObservation &) = delete;

    void record(double loadReactionNm, double driveReactionNm, double appliedDriveRpm) {
        if (m_count == m_rows.size()) throw std::runtime_error("mechanical observation storage exhausted");
        const bool completed = m_count != 0;
        const auto &shaft = *m_engine.getOutputCrankshaft();
        double total = 0;
        const auto *links = m_simulator.*SimulatorAccess::rods();
        const auto *couplings = m_simulator.*SimulatorAccess::shafts();
        const auto *endpoints = m_simulator.*SimulatorAccess::endpoints();
        for (std::size_t i = 0; i < m_cylinders; ++i) {
            const auto &chamber = *m_engine.getChamber(static_cast<int>(i));
            const auto &rod = links[2*i+1];
            const bool direct = completed && rod.m_bodies[1] == &shaft.m_body;
            const double x = direct ? rod.F_t[0][1] : 0;
            const double y = direct ? rod.F_t[1][1] : 0;
            if (direct) total += x + y;
            m_cylinderRows[m_count*m_cylinders+i] = {
                chamber.m_system.pressure() / units::Pa, chamber.m_system.volume() / units::m3, chamber.m_system.n(),
                completed ? chamber.getLastTimestepIntakeFlow() / m_simulator.getTimestep() : 0,
                completed ? chamber.getLastTimestepExhaustFlow() / m_simulator.getTimestep() : 0,
                x / units::Nm, y / units::Nm};
        }
        for (std::size_t i = 0; i < m_couplings; ++i) {
            const double moment = completed ? couplings[i].F_t[0][0] : 0;
            total += moment;
            m_couplingRows[m_count*m_couplings+i] = moment / units::Nm;
        }
        for (std::size_t i = 0; i < m_exhausts; ++i)
            m_exhaustRows[m_count*m_exhausts+i] = completed ? endpoints[i] : 0;
        for (std::size_t i = 0; i < m_intakes; ++i)
            m_intakeRows[m_count*m_intakes+i] = completed ? m_engine.getIntake(static_cast<int>(i))->m_flowRate : 0;
        const double speed = shaft.m_body.v_theta;
        m_rows[m_count++] = {speed, std::abs(speed) * 60 / (2 * constants::pi), shaft.getCycleAngle(),
            m_engine.getSpeedControl(), m_engine.getThrottlePlateAngle(), m_engine.getDisplacement() / units::m3,
            total / units::Nm, loadReactionNm, driveReactionNm, appliedDriveRpm};
    }

    const std::vector<CylinderBinding> &bindings() const noexcept { return m_bindings; }
    const std::vector<BankCurves> &bankCurves() const noexcept { return m_bankCurves; }
    const std::vector<CurveDescriptor> &curves() const noexcept { return m_curves; }
    std::size_t ignitionCurve() const noexcept { return m_ignitionCurve; }
    std::uint64_t seed() const noexcept { return m_engine.getRandomSeed(); }
    int frequency() const noexcept { return m_simulator.getSimulationFrequency(); }
    std::size_t count() const noexcept { return m_count; }
    const MechanicalTick &tick(std::size_t index) const {
        return m_rows[offset(index, 0, 1)];
    }
    const CylinderTick &cylinder(std::size_t tick, std::size_t index) const {
        return m_cylinderRows[offset(tick, index, m_cylinders)];
    }
    double coupling(std::size_t tick, std::size_t index) const {
        return m_couplingRows[offset(tick, index, m_couplings)];
    }
    double exhaust(std::size_t tick, std::size_t index) const {
        return m_exhaustRows[offset(tick, index, m_exhausts)];
    }
    double intake(std::size_t tick, std::size_t index) const {
        return m_intakeRows[offset(tick, index, m_intakes)];
    }

private:
    std::size_t observeCurve(const Function *curve, std::vector<const Function *> &identities) {
        if (!curve) throw std::logic_error("prepared curve is missing");
        const auto found = std::find(identities.begin(), identities.end(), curve);
        if (found != identities.end()) return static_cast<std::size_t>(found - identities.begin());
        const auto extent = (*curve).*CurveAccess::size();
        if (extent < 0 || extent > 100000 || m_curvePoints + extent > 1000000)
            throw std::invalid_argument("prepared curve extent exceeds the observation bound");
        m_curvePoints += extent;
        CurveDescriptor value{(*curve).*CurveAccess::radius(), (*curve).*CurveAccess::inputScale(),
                              (*curve).*CurveAccess::outputScale(), {}};
        const auto *x = (*curve).*CurveAccess::x();
        const auto *y = (*curve).*CurveAccess::y();
        value.points.reserve(extent);
        for (int i = 0; i < extent; ++i) value.points.emplace_back(x[i], y[i]);
        identities.push_back(curve);
        m_curves.push_back(std::move(value));
        return m_curves.size()-1;
    }
    std::size_t offset(std::size_t tick, std::size_t index, std::size_t stride) const {
        if (tick >= m_count || index >= stride) throw std::out_of_range("mechanical observation index");
        return tick*stride+index;
    }

    template<class T, class Get>
    static int componentIndex(const T *pointer, int count, Get get) {
        if (pointer == nullptr) return -1;
        for (int i = 0; i < count; ++i)
            if (pointer == get(i)) return i;
        throw std::logic_error("foreign component in prepared observation");
    }

    const PistonEngineSimulator &m_simulator;
    const Engine &m_engine;
    std::size_t m_cylinders, m_couplings, m_exhausts, m_intakes;
    std::size_t m_count = 0, m_curvePoints = 0, m_ignitionCurve = 0;
    std::vector<CylinderBinding> m_bindings;
    std::vector<CurveDescriptor> m_curves;
    std::vector<BankCurves> m_bankCurves;
    std::vector<MechanicalTick> m_rows;
    std::vector<CylinderTick> m_cylinderRows;
    std::vector<double> m_couplingRows, m_exhaustRows, m_intakeRows;
};
}
#endif
