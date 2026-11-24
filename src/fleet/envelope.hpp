#ifndef ENGINE_SIM_PRIVATE_FLEET_ENVELOPE_HPP
#define ENGINE_SIM_PRIVATE_FLEET_ENVELOPE_HPP

#include "engine_sim/fleet.h"

#include <cstdint>

namespace engine_sim::fleet {

inline std::uint32_t fadeFrames(const es_rt_event &event, std::uint32_t rate) noexcept {
    const auto ms = event.duration_ms == ES_RT_DFLT_MS ? (event.kind == ES_RT_REPLACE ? 20u : 5u) : event.duration_ms;
    return (ms * rate + 999) / 1000;
}

class Envelope {
public:
    explicit Envelope(double value = 0) noexcept : origin(value), goal(value) {}

    double at(std::uint64_t frame) const noexcept {
        if (frame >= end) return goal;
        if (frame <= start) return origin;
        return origin + (goal - origin) *
            (static_cast<double>(frame - start) / static_cast<double>(end - start));
    }
    double target() const noexcept { return goal; }
    std::uint64_t endpoint() const noexcept { return end; }

    // The coordinator has checked frame + frames, and frame order is monotonic.
    void retarget(std::uint64_t frame, std::uint32_t frames, double value) noexcept {
        origin = at(frame);
        goal = value;
        start = frame;
        end = origin == value ? frame : frame + frames;
    }

private:
    double origin, goal;
    std::uint64_t start = 0, end = 0;
};

inline constexpr std::uint32_t faultFrames = 32;

inline double faultSample(float last, std::uint64_t elapsed) noexcept {
    if (elapsed >= faultFrames - 1 || last == 0.f) return 0.;
    return static_cast<double>(last) * static_cast<double>(faultFrames - 1 - elapsed) / faultFrames;
}

inline double crossfade(double from, double to, double weight) noexcept {
    if (weight == 0.) return from;
    if (weight == 1.) return to;
    return (1. - weight) * from + weight * to;
}

inline float scaleStem(double sample, double gain, double gate) noexcept {
    if (gain == 0 || gate == 0) return 0.f;
    if (gain == 1 && gate == 1) return static_cast<float>(sample);
    return static_cast<float>(sample * gain * gate);
}

}
#endif
