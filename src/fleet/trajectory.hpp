#ifndef ENGINE_SIM_PRIVATE_FLEET_TRAJECTORY_HPP
#define ENGINE_SIM_PRIVATE_FLEET_TRAJECTORY_HPP

#include "engine_sim/fleet.h"

#include <cmath>

namespace engine_sim::fleet {

// A frame event replaces the complete target. Only a numerical tick evaluates
// the values applied to the independent engine.
class GameControls {
public:
    GameControls(es_rt_vector initial, std::uint32_t frequency, bool governor) noexcept
        : m_demand(initial.demand), m_load(initial.load_nm), m_speed(initial.driven_rpm),
          m_frequency(frequency), m_demandTicks(governor ? 0 : ticks(frequency, 10)),
          m_loadTicks(ticks(frequency, 5)) {}

    es_rt_vector at(std::uint64_t tick, es_rt_vector target) noexcept {
        target.demand = linear(m_demand, target.demand, tick, m_demandTicks);
        target.load_nm = linear(m_load, target.load_nm, tick, m_loadTicks);
        if (target.driven_rpm != m_speed.target) {
            m_speed = {speed(tick), target.driven_rpm, tick};
        }
        target.driven_rpm = speed(tick);
        return target;
    }

private:
    struct Segment {
        double origin, target;
        std::uint64_t start = 0;
        explicit Segment(double value) : origin(value), target(value) {}
        Segment(double value, double end, std::uint64_t tick) : origin(value), target(end), start(tick) {}

        double linearAt(std::uint64_t tick, std::uint32_t duration) const noexcept {
            const auto elapsed = tick - start;
            if (elapsed >= duration) return target;
            if (elapsed == 0) return origin;
            return origin + (target - origin) * (static_cast<double>(elapsed) / duration);
        }
    };

    static std::uint32_t ticks(std::uint32_t frequency, std::uint32_t milliseconds) noexcept {
        return (frequency * milliseconds + 999) / 1000;
    }

    static double linear(Segment &segment, double target, std::uint64_t tick, std::uint32_t duration) noexcept {
        if (target != segment.target) {
            segment = {segment.linearAt(tick, duration), target, tick};
        }
        return segment.linearAt(tick, duration);
    }

    double speed(std::uint64_t tick) const noexcept {
        const auto elapsed = tick - m_speed.start;
        const auto step = 2400. * static_cast<double>(elapsed) / m_frequency;
        const auto distance = m_speed.target - m_speed.origin;
        if (step >= std::abs(distance)) return m_speed.target;
        return m_speed.origin + std::copysign(step, distance);
    }

    Segment m_demand, m_load, m_speed;
    const std::uint32_t m_frequency, m_demandTicks, m_loadTicks;
};

}
#endif
