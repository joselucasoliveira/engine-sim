#include "read.hpp"
#include "schema.hpp"

#include <utility>

namespace engine_sim::config_v1::json_detail {
namespace {

template<class T> struct Decode;

template<class T>
T decode(Value value) {
    return Decode<T>::from(value);
}

template<class T>
struct Decode<Ref<T>> {
    static Ref<T> from(Value value) { return {std::string(value.text())}; }
};

template<class T>
struct Decode<Measure<T>> {
    static Measure<T> from(Value value) { return {value.node().real}; }
};

template<>
struct Decode<std::string> {
    static std::string from(Value value) { return std::string(value.text()); }
};

template<>
struct Decode<bool> {
    static bool from(Value value) { return value.node().boolean; }
};

template<>
struct Decode<std::int32_t> {
    static std::int32_t from(Value value) {
        // The pinned v1 rules have already applied the exact signed bounds.
        return integer32(value.json, value.node()).value;
    }
};

template<class T>
struct Decode<std::vector<T>> {
    static std::vector<T> from(Value value) {
        std::vector<T> output;
        std::size_t count = 0;
        for (auto i = value.index + 1; i < value.node().after; i = value.json.nodes[i].after) ++count;
        output.reserve(count);
        for (auto i = value.index + 1; i < value.node().after; i = value.json.nodes[i].after)
            output.push_back(decode<T>({value.json, i}));
        return output;
    }
};

template<class T>
void assign(Value object, std::string_view name, T &output) {
    output = decode<T>(object.member(name).value());
}

template<class T>
void assign(Value object, std::string_view name, std::optional<T> &output) {
    if (const auto value = object.member(name)) output = decode<T>(*value);
}

template<>
struct Decode<Sample> {
    static Sample from(Value value) {
        Sample output;
        assign(value, "x", output.x);
        assign(value, "y", output.y);
        return output;
    }
};

template<>
struct Decode<Function> {
    static Function from(Value value) {
        Function output;
        assign(value, "id", output.id);
        assign(value, "filter_radius", output.filter_radius);
        assign(value, "samples", output.samples);
        return output;
    }
};

template<>
struct Decode<RodJournal> {
    static RodJournal from(Value value) {
        RodJournal output;
        assign(value, "id", output.id);
        assign(value, "angle", output.angle);
        return output;
    }
};

template<>
struct Decode<Crankshaft> {
    static Crankshaft from(Value value) {
        Crankshaft output;
        assign(value, "id", output.id);
        assign(value, "throw", output.crank_throw);
        assign(value, "flywheel_mass", output.flywheel_mass);
        assign(value, "mass", output.mass);
        assign(value, "friction_torque", output.friction_torque);
        assign(value, "moment_of_inertia", output.moment_of_inertia);
        assign(value, "position_x", output.position_x);
        assign(value, "position_y", output.position_y);
        assign(value, "tdc", output.tdc);
        assign(value, "journals", output.journals);
        return output;
    }
};

template<>
struct Decode<Piston> {
    static Piston from(Value value) {
        Piston output;
        assign(value, "id", output.id);
        assign(value, "mass", output.mass);
        assign(value, "blowby", output.blowby);
        assign(value, "compression_height", output.compression_height);
        assign(value, "wrist_pin_position", output.wrist_pin_position);
        assign(value, "displacement", output.displacement);
        return output;
    }
};

template<>
struct Decode<ConnectingRod> {
    static ConnectingRod from(Value value) {
        ConnectingRod output;
        assign(value, "id", output.id);
        assign(value, "mass", output.mass);
        assign(value, "moment_of_inertia", output.moment_of_inertia);
        assign(value, "center_of_mass", output.center_of_mass);
        assign(value, "length", output.length);
        assign(value, "slave_throw", output.slave_throw);
        assign(value, "journals", output.journals);
        return output;
    }
};

template<>
struct Decode<Fuel> {
    static Fuel from(Value value) {
        Fuel output;
        assign(value, "id", output.id);
        assign(value, "name", output.name);
        assign(value, "molecular_mass", output.molecular_mass);
        assign(value, "energy_density", output.energy_density);
        assign(value, "density", output.density);
        assign(value, "molecular_afr", output.molecular_afr);
        assign(value, "turbulence_to_flame_speed_ratio", output.turbulence_to_flame_speed_ratio);
        assign(value, "max_burning_efficiency", output.max_burning_efficiency);
        assign(value, "burning_efficiency_randomness", output.burning_efficiency_randomness);
        assign(value, "low_efficiency_attenuation", output.low_efficiency_attenuation);
        assign(value, "max_turbulence_effect", output.max_turbulence_effect);
        assign(value, "max_dilution_effect", output.max_dilution_effect);
        return output;
    }
};

template<>
struct Decode<DirectThrottle> {
    static DirectThrottle from(Value value) {
        DirectThrottle output;
        assign(value, "id", output.id);
        assign(value, "gamma", output.gamma);
        return output;
    }
};

template<>
struct Decode<Governor> {
    static Governor from(Value value) {
        Governor output;
        assign(value, "id", output.id);
        assign(value, "min_speed", output.min_speed);
        assign(value, "max_speed", output.max_speed);
        assign(value, "min_v", output.min_v);
        assign(value, "max_v", output.max_v);
        assign(value, "k_s", output.k_s);
        assign(value, "k_d", output.k_d);
        assign(value, "gamma", output.gamma);
        return output;
    }
};

template<>
struct Decode<Engine> {
    static Engine from(Value value) {
        Engine output;
        assign(value, "id", output.id);
        assign(value, "name", output.name);
        assign(value, "redline", output.redline);
        assign(value, "starter_speed", output.starter_speed);
        assign(value, "starter_torque", output.starter_torque);
        assign(value, "dyno_min_speed", output.dyno_min_speed);
        assign(value, "dyno_max_speed", output.dyno_max_speed);
        assign(value, "dyno_hold_step", output.dyno_hold_step);
        assign(value, "fuel", output.fuel);
        assign(value, "throttle", output.throttle);
        assign(value, "simulation_frequency", output.simulation_frequency);
        assign(value, "hf_gain", output.hf_gain);
        assign(value, "jitter", output.jitter);
        assign(value, "noise", output.noise);
        assign(value, "crankshafts", output.crankshafts);
        assign(value, "banks", output.banks);
        assign(value, "ignition", output.ignition);
        assign(value, "mean_piston_speed_to_turbulence", output.mean_piston_speed_to_turbulence);
        return output;
    }
};

template<>
struct Decode<Cylinder> {
    static Cylinder from(Value value) {
        Cylinder output;
        assign(value, "piston", output.piston);
        assign(value, "connecting_rod", output.connecting_rod);
        assign(value, "rod_journal", output.rod_journal);
        assign(value, "intake", output.intake);
        assign(value, "exhaust_system", output.exhaust_system);
        assign(value, "ignition_wire", output.ignition_wire);
        assign(value, "primary_length", output.primary_length);
        assign(value, "sound_attenuation", output.sound_attenuation);
        return output;
    }
};

template<>
struct Decode<CylinderBank> {
    static CylinderBank from(Value value) {
        CylinderBank output;
        assign(value, "id", output.id);
        assign(value, "angle", output.angle);
        assign(value, "bore", output.bore);
        assign(value, "deck_height", output.deck_height);
        assign(value, "position_x", output.position_x);
        assign(value, "position_y", output.position_y);
        assign(value, "display_depth", output.display_depth);
        assign(value, "cylinders", output.cylinders);
        assign(value, "head", output.head);
        return output;
    }
};

template<>
struct Decode<Head> {
    static Head from(Value value) {
        Head output;
        assign(value, "id", output.id);
        assign(value, "chamber_volume", output.chamber_volume);
        assign(value, "intake_runner_volume", output.intake_runner_volume);
        assign(value, "intake_runner_cross_section_area", output.intake_runner_cross_section_area);
        assign(value, "exhaust_runner_volume", output.exhaust_runner_volume);
        assign(value, "exhaust_runner_cross_section_area", output.exhaust_runner_cross_section_area);
        assign(value, "flip_display", output.flip_display);
        assign(value, "intake_port_flow", output.intake_port_flow);
        assign(value, "exhaust_port_flow", output.exhaust_port_flow);
        assign(value, "valvetrain", output.valvetrain);
        return output;
    }
};

template<>
struct Decode<Standard> {
    static Standard from(Value value) {
        Standard output;
        assign(value, "id", output.id);
        assign(value, "intake_camshaft", output.intake_camshaft);
        assign(value, "exhaust_camshaft", output.exhaust_camshaft);
        return output;
    }
};

template<>
struct Decode<Vtec> {
    static Vtec from(Value value) {
        Vtec output;
        assign(value, "id", output.id);
        assign(value, "intake_camshaft", output.intake_camshaft);
        assign(value, "exhaust_camshaft", output.exhaust_camshaft);
        assign(value, "vtec_intake_camshaft", output.vtec_intake_camshaft);
        assign(value, "vtec_exhaust_camshaft", output.vtec_exhaust_camshaft);
        assign(value, "min_rpm", output.min_rpm);
        assign(value, "min_speed", output.min_speed);
        assign(value, "manifold_vacuum", output.manifold_vacuum);
        assign(value, "min_throttle_position", output.min_throttle_position);
        return output;
    }
};

template<>
struct Decode<Camshaft> {
    static Camshaft from(Value value) {
        Camshaft output;
        assign(value, "id", output.id);
        assign(value, "advance", output.advance);
        assign(value, "base_radius", output.base_radius);
        assign(value, "lobe_profile", output.lobe_profile);
        assign(value, "lobes", output.lobes);
        return output;
    }
};

template<>
struct Decode<Intake> {
    static Intake from(Value value) {
        Intake output;
        assign(value, "id", output.id);
        assign(value, "plenum_volume", output.plenum_volume);
        assign(value, "plenum_cross_section_area", output.plenum_cross_section_area);
        assign(value, "intake_flow_rate", output.intake_flow_rate);
        assign(value, "idle_flow_rate", output.idle_flow_rate);
        assign(value, "runner_flow_rate", output.runner_flow_rate);
        assign(value, "molecular_afr", output.molecular_afr);
        assign(value, "idle_throttle_plate_position", output.idle_throttle_plate_position);
        assign(value, "runner_length", output.runner_length);
        assign(value, "velocity_decay", output.velocity_decay);
        return output;
    }
};

template<>
struct Decode<ExhaustSystem> {
    static ExhaustSystem from(Value value) {
        ExhaustSystem output;
        assign(value, "id", output.id);
        assign(value, "length", output.length);
        assign(value, "collector_cross_section_area", output.collector_cross_section_area);
        assign(value, "outlet_flow_rate", output.outlet_flow_rate);
        assign(value, "primary_tube_length", output.primary_tube_length);
        assign(value, "primary_flow_rate", output.primary_flow_rate);
        assign(value, "audio_volume", output.audio_volume);
        assign(value, "velocity_decay", output.velocity_decay);
        assign(value, "impulse_response", output.impulse_response);
        return output;
    }
};

template<>
struct Decode<ImpulseResponse> {
    static ImpulseResponse from(Value value) {
        ImpulseResponse output;
        assign(value, "id", output.id);
        assign(value, "path", output.path);
        assign(value, "volume", output.volume);
        return output;
    }
};

template<>
struct Decode<Post> {
    static Post from(Value value) {
        Post output;
        assign(value, "wire", output.wire);
        assign(value, "angle", output.angle);
        return output;
    }
};

template<>
struct Decode<Wire> {
    static Wire from(Value value) {
        Wire output;
        assign(value, "id", output.id);
        return output;
    }
};

template<>
struct Decode<Ignition> {
    static Ignition from(Value value) {
        Ignition output;
        assign(value, "id", output.id);
        assign(value, "timing_curve", output.timing_curve);
        assign(value, "rev_limit", output.rev_limit);
        assign(value, "limiter_duration", output.limiter_duration);
        assign(value, "posts", output.posts);
        return output;
    }
};

template<>
struct Decode<Vehicle> {
    static Vehicle from(Value value) {
        Vehicle output;
        assign(value, "mass", output.mass);
        assign(value, "drag_coefficient", output.drag_coefficient);
        assign(value, "cross_sectional_area", output.cross_sectional_area);
        assign(value, "diff_ratio", output.diff_ratio);
        assign(value, "tire_radius", output.tire_radius);
        assign(value, "rolling_resistance", output.rolling_resistance);
        return output;
    }
};

template<>
struct Decode<Transmission> {
    static Transmission from(Value value) {
        Transmission output;
        assign(value, "max_clutch_torque", output.max_clutch_torque);
        assign(value, "gears", output.gears);
        return output;
    }
};

template<>
struct Decode<Desktop> {
    static Desktop from(Value value) {
        Desktop output;
        assign(value, "start_fullscreen", output.start_fullscreen);
        assign(value, "power_units", output.power_units);
        assign(value, "torque_units", output.torque_units);
        assign(value, "speed_units", output.speed_units);
        assign(value, "pressure_units", output.pressure_units);
        assign(value, "boost_units", output.boost_units);
        assign(value, "color_background", output.color_background);
        assign(value, "color_foreground", output.color_foreground);
        assign(value, "color_shadow", output.color_shadow);
        assign(value, "color_highlight1", output.color_highlight1);
        assign(value, "color_highlight2", output.color_highlight2);
        assign(value, "color_pink", output.color_pink);
        assign(value, "color_red", output.color_red);
        assign(value, "color_orange", output.color_orange);
        assign(value, "color_yellow", output.color_yellow);
        assign(value, "color_blue", output.color_blue);
        assign(value, "color_green", output.color_green);
        return output;
    }
};

template<>
struct Decode<Initialization> {
    static Initialization from(Value) { return {}; }
};

template<>
struct Decode<Throttle> {
    static Throttle from(Value value) {
        if (value.member("kind").value().text() == DirectThrottle::kind) return decode<DirectThrottle>(value);
        return decode<Governor>(value);
    }
};

template<>
struct Decode<Valvetrain> {
    static Valvetrain from(Value value) {
        if (value.member("kind").value().text() == Standard::kind) return decode<Standard>(value);
        return decode<Vtec>(value);
    }
};

template<>
struct Decode<Components> {
    static Components from(Value value) {
        Components output;
        assign(value, "functions", output.functions);
        assign(value, "journals", output.journals);
        assign(value, "crankshafts", output.crankshafts);
        assign(value, "pistons", output.pistons);
        assign(value, "rods", output.rods);
        assign(value, "fuels", output.fuels);
        assign(value, "throttles", output.throttles);
        assign(value, "banks", output.banks);
        assign(value, "heads", output.heads);
        assign(value, "valvetrains", output.valvetrains);
        assign(value, "camshafts", output.camshafts);
        assign(value, "intakes", output.intakes);
        assign(value, "exhausts", output.exhausts);
        assign(value, "impulses", output.impulses);
        assign(value, "wires", output.wires);
        assign(value, "ignitions", output.ignitions);
        return output;
    }
};

template<>
struct Decode<Document> {
    static Document from(Value value) {
        Document output;
        assign(value, "selected_engine", output.selected_engine);
        assign(value, "engine", output.engine);
        assign(value, "components", output.components);
        assign(value, "initialization", output.initialization);
        assign(value, "vehicle", output.vehicle);
        assign(value, "transmission", output.transmission);
        assign(value, "desktop", output.desktop);
        return output;
    }
};

}

AuthoredResult read_authored(std::string_view input) {
    auto result = parse(input);
    if (auto *error = std::get_if<ParseError>(&result)) return std::move(error->diagnostic);
    const auto &parsed = std::get<ParsedJson>(result);
    if (auto problem = validate_v1(parsed)) return std::move(*problem);
    return decode<Document>({parsed, 0});
}

ModelResult read_normalized(std::string_view input) {
    auto result = parse(input);
    if (auto *error = std::get_if<ParseError>(&result)) return std::move(error->diagnostic);
    const auto &parsed = std::get<ParsedJson>(result);
    if (auto problem = validate_v1(parsed)) return std::move(*problem);
    auto model = normalize(decode<Document>({parsed, 0}));
    if (auto *problem = std::get_if<Diagnostic>(&model)) locate_diagnostic(parsed, *problem);
    return model;
}

}
