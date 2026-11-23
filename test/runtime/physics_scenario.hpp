#ifndef ENGINE_SIM_PHYSICS_TEST_SCENARIO_HPP
#define ENGINE_SIM_PHYSICS_TEST_SCENARIO_HPP

#include "piston_engine_simulator.h"
#include "units.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace physics_test {
constexpr int Frequency = 13333;
constexpr std::size_t Ticks = 24007;
constexpr std::uint64_t Seed = 403;
struct Command {
    std::size_t tick;
    double demand, load;
    bool ignition, starter;
};
constexpr std::array<Command, 5> Commands{{
    {0, 0.20, 0.0, true, true}, {4000, 0.35, 0.0, true, true},
    {9500, 0.45, 3.25, true, false}, {16000, 0.30, 1.25, false, false},
    {19000, 0.20, 0.0, true, false},
}};

struct ReadConstraints : PistonEngineSimulator {
    static auto rodLinks() { return &ReadConstraints::m_linkConstraints; }
};

using Record = std::array<double, 21>;
inline Record observe(const PistonEngineSimulator &sim, double excitation, bool completed) {
    const auto &engine = *sim.getEngine();
    const auto &crank = *engine.getOutputCrankshaft();
    const auto &chamber = *engine.getChamber(0);
    const auto &body = crank.m_body;
    const auto &link = (sim.*ReadConstraints::rodLinks())[1];
    const auto first = completed ? link.F_t[0][1] : 0.0;
    const auto second = completed ? link.F_t[1][1] : 0.0;
    return {
        engine.getDisplacement() / units::m3, engine.getRpm(), body.v_theta,
        body.theta, crank.getCycleAngle(), body.p_x, body.p_y,
        chamber.m_system.pressure() / units::Pa, chamber.m_system.volume() / units::m3,
        chamber.m_system.n(), chamber.m_exhaustRunnerAndPrimary.pressure() / units::Pa,
        engine.getManifoldPressure() / units::Pa, engine.getThrottlePlateAngle(),
        (first + second) / units::Nm, first / units::Nm, second / units::Nm,
        sim.getExternalLoadReactionNm(), excitation, double(sim.m_starterMotor.m_enabled),
        double(engine.getIgnitionModule()->m_enabled), double(chamber.isLit())
    };
}

inline void save(std::ofstream &file, const Record &row) {
    for (const auto value : row) {
        if (!std::isfinite(value)) throw std::runtime_error("Nonfinite observed physics");
        std::uint64_t word;
        static_assert(sizeof word == sizeof value);
        std::memcpy(&word, &value, sizeof word);
        for (unsigned shift = 0; shift < 64; shift += 8) file.put(static_cast<char>(word >> shift));
    }
    if (!file) throw std::runtime_error("Cannot save physics witness");
}
}

#endif
