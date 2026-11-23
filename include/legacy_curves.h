#ifndef ENGINE_SIM_LEGACY_CURVES_H
#define ENGINE_SIM_LEGACY_CURVES_H

#include "units.h"

#include <cmath>

namespace engine_sim::legacy_curves {

struct HarmonicInputs {
    double duration_at_50_thou_rad;
    double gamma;
    double lift_m;
    int steps;
};

struct HarmonicCamLobe {
    HarmonicInputs input;
    double cosine_at_50_thou;
    double frequency;
    double extent;
    double step;

    template<class AddSample>
    void sample(AddSample add) const {
        add(0.0, input.lift_m);
        for (int i = 1; i < input.steps; ++i) {
            const double x = i * step;
            const double lift = (x >= extent)
                ? 0.0
                : input.lift_m * std::pow(0.5 + 0.5 * std::cos(frequency * x), input.gamma);
            add(x, lift);
            add(-x, lift);
        }
    }
};

inline HarmonicCamLobe harmonic_cam_lobe(HarmonicInputs input) {
    const double angle = input.duration_at_50_thou_rad / 4;
    const double s = std::pow(2 * units::distance(50, units::thou) / input.lift_m, 1 / input.gamma) - 1;
    const double k = std::acos(s) / angle;
    const double extents = constants::pi / k;
    const double step = extents / (input.steps - 5.0);
    return {input, s, k, extents, step};
}

inline double flow_constant(
    double targetFlowRate,
    double P,
    double pressureDrop,
    double T,
    double hcr)
{
    const double T_0 = T;
    const double p_0 = P, p_T = P - pressureDrop; // p_0 = upstream pressure

    const double chokedFlowLimit =
        std::pow((2.0 / (hcr + 1)), hcr / (hcr - 1));
    const double p_ratio = p_T / p_0;

    double flowRate = 0;
    if (p_ratio <= chokedFlowLimit) {
        // Choked flow
        flowRate = std::sqrt(hcr);
        flowRate *= std::pow(2 / (hcr + 1), (hcr + 1) / (2 * (hcr - 1)));
    }
    else {
        flowRate = (2 * hcr) / (hcr - 1);
        flowRate *= (1 - std::pow(p_ratio, (hcr - 1) / hcr));
        flowRate = std::sqrt(flowRate);
        flowRate *= std::pow(p_ratio, 1 / hcr);
    }

    flowRate *= p_0 / std::sqrt(constants::R * T_0);

    return targetFlowRate / flowRate;
}
inline double port_flow_28_inh2o(double flowRateScfm) {
    return flow_constant(
        units::flow(flowRateScfm, units::scfm),
        units::pressure(1.0, units::atm),
        units::pressure(28.0, units::inH2O),
        units::celcius(25),
        1.0 + (2.0 / 5)
    );
}

struct DefaultTurbulence {
    static constexpr int samples = 30;
    static constexpr double radius = 1.0;

    template<class AddSample>
    static void sample(AddSample add) {
        for (int i = 0; i < samples; ++i) {
            const double speed = static_cast<double>(i);
            add(speed, speed * 0.5);
        }
    }
};
}
#endif
