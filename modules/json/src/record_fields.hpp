#ifndef ENGINE_SIM_JSON_RECORD_FIELDS_HPP
#define ENGINE_SIM_JSON_RECORD_FIELDS_HPP

#include <engine_sim/config-v1.hpp>

namespace engine_sim::config_v1::json_detail {

// The v1 wire keys are in ascending byte order. Array members retain their
// original order. Static const members carry fixed wire constants.
template<class Record> struct RecordFields;

template<>
struct RecordFields<Sample> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("x", record.x);
        visit("y", record.y);
    }
};

template<>
struct RecordFields<Function> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("filter_radius", record.filter_radius);
        visit("id", record.id);
        visit("input_scale", Function::input_scale);
        visit("output_scale", Function::output_scale);
        visit("samples", record.samples);
    }
};

template<>
struct RecordFields<RodJournal> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("angle", record.angle);
        visit("id", record.id);
    }
};

template<>
struct RecordFields<Crankshaft> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("flywheel_mass", record.flywheel_mass);
        visit("friction_torque", record.friction_torque);
        visit("id", record.id);
        visit("journals", record.journals);
        visit("mass", record.mass);
        visit("moment_of_inertia", record.moment_of_inertia);
        visit("position_x", record.position_x);
        visit("position_y", record.position_y);
        visit("tdc", record.tdc);
        visit("throw", record.crank_throw);
    }
};

template<>
struct RecordFields<Piston> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("blowby", record.blowby);
        visit("compression_height", record.compression_height);
        visit("displacement", record.displacement);
        visit("id", record.id);
        visit("mass", record.mass);
        visit("wrist_pin_position", record.wrist_pin_position);
    }
};

template<>
struct RecordFields<ConnectingRod> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("center_of_mass", record.center_of_mass);
        visit("id", record.id);
        visit("journals", record.journals);
        visit("length", record.length);
        visit("mass", record.mass);
        visit("moment_of_inertia", record.moment_of_inertia);
        visit("slave_throw", record.slave_throw);
    }
};

template<>
struct RecordFields<Fuel> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("burning_efficiency_randomness", record.burning_efficiency_randomness);
        visit("density", record.density);
        visit("energy_density", record.energy_density);
        visit("id", record.id);
        visit("low_efficiency_attenuation", record.low_efficiency_attenuation);
        visit("max_burning_efficiency", record.max_burning_efficiency);
        visit("max_dilution_effect", record.max_dilution_effect);
        visit("max_turbulence_effect", record.max_turbulence_effect);
        visit("molecular_afr", record.molecular_afr);
        visit("molecular_mass", record.molecular_mass);
        visit("name", record.name);
        visit("turbulence_to_flame_speed_ratio", record.turbulence_to_flame_speed_ratio);
    }
};

template<>
struct RecordFields<DirectThrottle> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("gamma", record.gamma);
        visit("id", record.id);
        visit("kind", DirectThrottle::kind);
    }
};

template<>
struct RecordFields<Governor> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("gamma", record.gamma);
        visit("id", record.id);
        visit("k_d", record.k_d);
        visit("k_s", record.k_s);
        visit("kind", Governor::kind);
        visit("max_speed", record.max_speed);
        visit("max_v", record.max_v);
        visit("min_speed", record.min_speed);
        visit("min_v", record.min_v);
    }
};

template<>
struct RecordFields<Engine> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("banks", record.banks);
        visit("crankshafts", record.crankshafts);
        visit("dyno_hold_step", record.dyno_hold_step);
        visit("dyno_max_speed", record.dyno_max_speed);
        visit("dyno_min_speed", record.dyno_min_speed);
        visit("fuel", record.fuel);
        visit("hf_gain", record.hf_gain);
        visit("id", record.id);
        visit("ignition", record.ignition);
        visit("jitter", record.jitter);
        visit("mean_piston_speed_to_turbulence", record.mean_piston_speed_to_turbulence);
        visit("name", record.name);
        visit("noise", record.noise);
        visit("redline", record.redline);
        visit("simulation_frequency", record.simulation_frequency);
        visit("starter_speed", record.starter_speed);
        visit("starter_torque", record.starter_torque);
        visit("throttle", record.throttle);
    }
};

template<>
struct RecordFields<Cylinder> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("connecting_rod", record.connecting_rod);
        visit("exhaust_system", record.exhaust_system);
        visit("ignition_wire", record.ignition_wire);
        visit("intake", record.intake);
        visit("piston", record.piston);
        visit("primary_length", record.primary_length);
        visit("rod_journal", record.rod_journal);
        visit("sound_attenuation", record.sound_attenuation);
    }
};

template<>
struct RecordFields<CylinderBank> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("angle", record.angle);
        visit("bore", record.bore);
        visit("cylinders", record.cylinders);
        visit("deck_height", record.deck_height);
        visit("display_depth", record.display_depth);
        visit("head", record.head);
        visit("id", record.id);
        visit("position_x", record.position_x);
        visit("position_y", record.position_y);
    }
};

template<>
struct RecordFields<Head> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("chamber_volume", record.chamber_volume);
        visit("exhaust_port_flow", record.exhaust_port_flow);
        visit("exhaust_runner_cross_section_area", record.exhaust_runner_cross_section_area);
        visit("exhaust_runner_volume", record.exhaust_runner_volume);
        visit("flip_display", record.flip_display);
        visit("id", record.id);
        visit("intake_port_flow", record.intake_port_flow);
        visit("intake_runner_cross_section_area", record.intake_runner_cross_section_area);
        visit("intake_runner_volume", record.intake_runner_volume);
        visit("valvetrain", record.valvetrain);
    }
};

template<>
struct RecordFields<Standard> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("exhaust_camshaft", record.exhaust_camshaft);
        visit("id", record.id);
        visit("intake_camshaft", record.intake_camshaft);
        visit("kind", Standard::kind);
    }
};

template<>
struct RecordFields<Vtec> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("exhaust_camshaft", record.exhaust_camshaft);
        visit("id", record.id);
        visit("intake_camshaft", record.intake_camshaft);
        visit("kind", Vtec::kind);
        visit("manifold_vacuum", record.manifold_vacuum);
        visit("min_rpm", record.min_rpm);
        visit("min_speed", record.min_speed);
        visit("min_throttle_position", record.min_throttle_position);
        visit("vtec_exhaust_camshaft", record.vtec_exhaust_camshaft);
        visit("vtec_intake_camshaft", record.vtec_intake_camshaft);
    }
};

template<>
struct RecordFields<Camshaft> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("advance", record.advance);
        visit("base_radius", record.base_radius);
        visit("id", record.id);
        visit("lobe_profile", record.lobe_profile);
        visit("lobes", record.lobes);
    }
};

template<>
struct RecordFields<Intake> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("id", record.id);
        visit("idle_flow_rate", record.idle_flow_rate);
        visit("idle_throttle_plate_position", record.idle_throttle_plate_position);
        visit("intake_flow_rate", record.intake_flow_rate);
        visit("molecular_afr", record.molecular_afr);
        visit("plenum_cross_section_area", record.plenum_cross_section_area);
        visit("plenum_volume", record.plenum_volume);
        visit("runner_flow_rate", record.runner_flow_rate);
        visit("runner_length", record.runner_length);
        visit("velocity_decay", record.velocity_decay);
    }
};

template<>
struct RecordFields<ExhaustSystem> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("audio_volume", record.audio_volume);
        visit("collector_cross_section_area", record.collector_cross_section_area);
        visit("id", record.id);
        visit("impulse_response", record.impulse_response);
        visit("length", record.length);
        visit("outlet_flow_rate", record.outlet_flow_rate);
        visit("primary_flow_rate", record.primary_flow_rate);
        visit("primary_tube_length", record.primary_tube_length);
        visit("velocity_decay", record.velocity_decay);
    }
};

template<>
struct RecordFields<ImpulseResponse> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("id", record.id);
        visit("path", record.path);
        visit("volume", record.volume);
    }
};

template<>
struct RecordFields<Post> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("angle", record.angle);
        visit("wire", record.wire);
    }
};

template<>
struct RecordFields<Wire> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("id", record.id);
    }
};

template<>
struct RecordFields<Ignition> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("id", record.id);
        visit("limiter_duration", record.limiter_duration);
        visit("posts", record.posts);
        visit("rev_limit", record.rev_limit);
        visit("timing_curve", record.timing_curve);
    }
};

template<>
struct RecordFields<Vehicle> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("cross_sectional_area", record.cross_sectional_area);
        visit("diff_ratio", record.diff_ratio);
        visit("drag_coefficient", record.drag_coefficient);
        visit("mass", record.mass);
        visit("rolling_resistance", record.rolling_resistance);
        visit("tire_radius", record.tire_radius);
    }
};

template<>
struct RecordFields<Transmission> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("gears", record.gears);
        visit("max_clutch_torque", record.max_clutch_torque);
    }
};

template<>
struct RecordFields<Desktop> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("boost_units", record.boost_units);
        visit("color_background", record.color_background);
        visit("color_blue", record.color_blue);
        visit("color_foreground", record.color_foreground);
        visit("color_green", record.color_green);
        visit("color_highlight1", record.color_highlight1);
        visit("color_highlight2", record.color_highlight2);
        visit("color_orange", record.color_orange);
        visit("color_pink", record.color_pink);
        visit("color_red", record.color_red);
        visit("color_shadow", record.color_shadow);
        visit("color_yellow", record.color_yellow);
        visit("power_units", record.power_units);
        visit("pressure_units", record.pressure_units);
        visit("speed_units", record.speed_units);
        visit("start_fullscreen", record.start_fullscreen);
        visit("torque_units", record.torque_units);
    }
};

template<>
struct RecordFields<Initialization> {
    template<class Record, class Visit>
    void operator()(Record &, Visit &&visit) const {
        visit("chamber_kelvin", Initialization::chamber_kelvin);
        visit("chamber_pa", Initialization::chamber_pa);
        visit("crankcase_pa", Initialization::crankcase_pa);
        visit("profile", Initialization::profile);
    }
};

template<>
struct RecordFields<Components> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("banks", record.banks);
        visit("camshafts", record.camshafts);
        visit("crankshafts", record.crankshafts);
        visit("exhausts", record.exhausts);
        visit("fuels", record.fuels);
        visit("functions", record.functions);
        visit("heads", record.heads);
        visit("ignitions", record.ignitions);
        visit("impulses", record.impulses);
        visit("intakes", record.intakes);
        visit("journals", record.journals);
        visit("pistons", record.pistons);
        visit("rods", record.rods);
        visit("throttles", record.throttles);
        visit("valvetrains", record.valvetrains);
        visit("wires", record.wires);
    }
};

template<>
struct RecordFields<Document> {
    template<class Record, class Visit>
    void operator()(Record &record, Visit &&visit) const {
        visit("components", record.components);
        visit("desktop", record.desktop);
        visit("engine", record.engine);
        visit("format_version", Document::format_version);
        visit("initialization", record.initialization);
        visit("selected_engine", record.selected_engine);
        visit("transmission", record.transmission);
        visit("vehicle", record.vehicle);
    }
};

}
#endif
