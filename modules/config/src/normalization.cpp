#include "normalization.hpp"

#include <cmath>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace engine_sim::config_v1::detail {
namespace {
constexpr double inch = 0.01 * 2.54;
constexpr double foot = inch * 12.0;
constexpr double vehicle_side = 6.0 * foot;
constexpr double foot_pound = foot * 4.44822;

Vehicle default_vehicle() {
    return {{1597.0}, {0.25}, {vehicle_side * vehicle_side}, {3.42}, {10.0 * inch}, {2000.0}};
}

Transmission default_transmission() {
    return {{1000.0 * foot_pound}, {{2.97}, {2.07}, {1.43}, {1.0}, {0.84}, {0.56}}};
}

Desktop default_desktop() {
    return {false, "hp", "lb-ft", "mph", "inHg", "psi",
            0x0E1012, 0xFFFFFF, 0x0E1012, 0xEF4545, 0xFFFFFF, 0xF394BE,
            0xEE4445, 0xF4802A, 0xFDBD2E, 0x77CEE0, 0xBDD869};
}

struct Location {
    std::string_view root;
    std::optional<std::size_t> record{};
    std::string_view member{};
    std::optional<std::size_t> element{};
    const std::string *component = nullptr;
};

std::string pointer(const Location &where, std::string_view field) {
    std::string path(where.root);
    if (where.record) { path += '/'; path += std::to_string(*where.record); }
    if (!where.member.empty()) { path += '/'; path += where.member; }
    if (where.element) { path += '/'; path += std::to_string(*where.element); }
    if (!field.empty()) { path += '/'; path += field; }
    return path;
}

Diagnostic diagnostic(const Location &where, std::string_view field,
                      std::string code, std::string stage, std::string message) {
    Diagnostic error;
    error.code = std::move(code);
    error.stage = std::move(stage);
    error.pointer = pointer(where, field);
    error.message = std::move(message);
    if (where.component) error.component_id = *where.component;
    if (!field.empty()) error.field = field;
    else if (!where.member.empty()) error.field = where.member;
    return error;
}

using Error = std::optional<Diagnostic>;
using Scalar = std::pair<std::string_view, double>;

Error finite(const Location &where, std::initializer_list<Scalar> fields) {
    for (const auto &[name, value] : fields) {
        if (!std::isfinite(value))
            return diagnostic(where, name, "CFG_NUMBER", "normalize", "Expected a finite binary64 value");
    }
    return std::nullopt;
}

template<class Unit>
Error finite_array(Location where, std::string_view member, const std::vector<Measure<Unit>> &values) {
    where.member = member;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!std::isfinite(values[i].value)) {
            where.element = i;
            return diagnostic(where, {}, "CFG_NUMBER", "normalize", "Expected a finite binary64 value");
        }
    }
    return std::nullopt;
}

Error check(const Sample &value, const Location &where) {
    return finite(where, {{"x", value.x.value},
        {"y", value.y.value}});
}

Error check(const Cylinder &value, const Location &where) {
    return finite(where, {{"primary_length", value.primary_length.value},
        {"sound_attenuation", value.sound_attenuation.value}});
}

Error check(const Post &value, const Location &where) {
    return finite(where, {{"angle", value.angle.value}});
}

Error check(const Function &value, Location where) {
    where.component = &value.id.value;
    if (auto error = finite(where, {{"filter_radius", value.filter_radius.value}})) return error;
    where.member = "samples";
    for (std::size_t i = 0; i < value.samples.size(); ++i) {
        where.element = i;
        if (auto error = check(value.samples[i], where)) return error;
    }
    return std::nullopt;
}

Error check(const RodJournal &value, Location where) {
    where.component = &value.id.value;
    return finite(where, {{"angle", value.angle.value}});
}

Error check(const Crankshaft &value, Location where) {
    where.component = &value.id.value;
    return finite(where, {{"crank_throw", value.crank_throw.value},
        {"flywheel_mass", value.flywheel_mass.value},
        {"mass", value.mass.value},
        {"friction_torque", value.friction_torque.value},
        {"moment_of_inertia", value.moment_of_inertia.value},
        {"position_x", value.position_x.value},
        {"position_y", value.position_y.value},
        {"tdc", value.tdc.value}});
}

Error check(const Piston &value, Location where) {
    where.component = &value.id.value;
    return finite(where, {{"mass", value.mass.value},
        {"blowby", value.blowby.value},
        {"compression_height", value.compression_height.value},
        {"wrist_pin_position", value.wrist_pin_position.value},
        {"displacement", value.displacement.value}});
}

Error check(const ConnectingRod &value, Location where) {
    where.component = &value.id.value;
    return finite(where, {{"mass", value.mass.value},
        {"moment_of_inertia", value.moment_of_inertia.value},
        {"center_of_mass", value.center_of_mass.value},
        {"length", value.length.value},
        {"slave_throw", value.slave_throw.value}});
}

Error check(const Fuel &value, Location where) {
    where.component = &value.id.value;
    return finite(where, {{"molecular_mass", value.molecular_mass.value},
        {"energy_density", value.energy_density.value},
        {"density", value.density.value},
        {"molecular_afr", value.molecular_afr.value},
        {"max_burning_efficiency", value.max_burning_efficiency.value},
        {"burning_efficiency_randomness", value.burning_efficiency_randomness.value},
        {"low_efficiency_attenuation", value.low_efficiency_attenuation.value},
        {"max_turbulence_effect", value.max_turbulence_effect.value},
        {"max_dilution_effect", value.max_dilution_effect.value}});
}

Error check(const DirectThrottle &value, Location where) {
    where.component = &value.id.value;
    return finite(where, {{"gamma", value.gamma.value}});
}

Error check(const Governor &value, Location where) {
    where.component = &value.id.value;
    return finite(where, {{"min_speed", value.min_speed.value},
        {"max_speed", value.max_speed.value},
        {"min_v", value.min_v.value},
        {"max_v", value.max_v.value},
        {"k_s", value.k_s.value},
        {"k_d", value.k_d.value},
        {"gamma", value.gamma.value}});
}

Error check(const Engine &value, Location where) {
    where.component = &value.id.value;
    return finite(where, {{"redline", value.redline.value},
        {"starter_speed", value.starter_speed.value},
        {"starter_torque", value.starter_torque.value},
        {"dyno_min_speed", value.dyno_min_speed.value},
        {"dyno_max_speed", value.dyno_max_speed.value},
        {"dyno_hold_step", value.dyno_hold_step.value},
        {"simulation_frequency", value.simulation_frequency.value},
        {"hf_gain", value.hf_gain.value},
        {"jitter", value.jitter.value},
        {"noise", value.noise.value}});
}

Error check(const CylinderBank &value, Location where) {
    where.component = &value.id.value;
    if (auto error = finite(where, {{"angle", value.angle.value},
        {"bore", value.bore.value},
        {"deck_height", value.deck_height.value},
        {"position_x", value.position_x.value},
        {"position_y", value.position_y.value},
        {"display_depth", value.display_depth.value}})) return error;
    where.member = "cylinders";
    for (std::size_t i = 0; i < value.cylinders.size(); ++i) {
        where.element = i;
        if (auto error = check(value.cylinders[i], where)) return error;
    }
    return std::nullopt;
}

Error check(const Head &value, Location where) {
    where.component = &value.id.value;
    return finite(where, {{"chamber_volume", value.chamber_volume.value},
        {"intake_runner_volume", value.intake_runner_volume.value},
        {"intake_runner_cross_section_area", value.intake_runner_cross_section_area.value},
        {"exhaust_runner_volume", value.exhaust_runner_volume.value},
        {"exhaust_runner_cross_section_area", value.exhaust_runner_cross_section_area.value}});
}

Error check(const Vtec &value, Location where) {
    where.component = &value.id.value;
    return finite(where, {{"min_rpm", value.min_rpm.value},
        {"min_speed", value.min_speed.value},
        {"manifold_vacuum", value.manifold_vacuum.value},
        {"min_throttle_position", value.min_throttle_position.value}});
}

Error check(const Camshaft &value, Location where) {
    where.component = &value.id.value;
    if (auto error = finite(where, {{"advance", value.advance.value},
        {"base_radius", value.base_radius.value}})) return error;
    return finite_array(where, "lobes", value.lobes);
}

Error check(const Intake &value, Location where) {
    where.component = &value.id.value;
    return finite(where, {{"plenum_volume", value.plenum_volume.value},
        {"plenum_cross_section_area", value.plenum_cross_section_area.value},
        {"intake_flow_rate", value.intake_flow_rate.value},
        {"idle_flow_rate", value.idle_flow_rate.value},
        {"runner_flow_rate", value.runner_flow_rate.value},
        {"molecular_afr", value.molecular_afr.value},
        {"idle_throttle_plate_position", value.idle_throttle_plate_position.value},
        {"runner_length", value.runner_length.value},
        {"velocity_decay", value.velocity_decay.value}});
}

Error check(const ExhaustSystem &value, Location where) {
    where.component = &value.id.value;
    return finite(where, {{"length", value.length.value},
        {"collector_cross_section_area", value.collector_cross_section_area.value},
        {"outlet_flow_rate", value.outlet_flow_rate.value},
        {"primary_tube_length", value.primary_tube_length.value},
        {"primary_flow_rate", value.primary_flow_rate.value},
        {"audio_volume", value.audio_volume.value},
        {"velocity_decay", value.velocity_decay.value}});
}

Error check(const ImpulseResponse &value, Location where) {
    where.component = &value.id.value;
    return finite(where, {{"volume", value.volume.value}});
}

Error check(const Ignition &value, Location where) {
    where.component = &value.id.value;
    if (auto error = finite(where, {{"rev_limit", value.rev_limit.value},
        {"limiter_duration", value.limiter_duration.value}})) return error;
    where.member = "posts";
    for (std::size_t i = 0; i < value.posts.size(); ++i) {
        where.element = i;
        if (auto error = check(value.posts[i], where)) return error;
    }
    return std::nullopt;
}

Error check(const Vehicle &value, const Location &where) {
    return finite(where, {{"mass", value.mass.value},
        {"drag_coefficient", value.drag_coefficient.value},
        {"cross_sectional_area", value.cross_sectional_area.value},
        {"diff_ratio", value.diff_ratio.value},
        {"tire_radius", value.tire_radius.value},
        {"rolling_resistance", value.rolling_resistance.value}});
}

Error check(const Transmission &value, const Location &where) {
    if (auto error = finite(where, {{"max_clutch_torque", value.max_clutch_torque.value}})) return error;
    if (value.gears.empty())
        return diagnostic(where, "gears", "CFG_SHAPE", "shape", "An explicit transmission requires gears");
    return finite_array(where, "gears", value.gears);
}

Error check(const Standard &, const Location &) { return std::nullopt; }

template<class... Alternatives>
Error check(const std::variant<Alternatives...> &value, const Location &where) {
    if (value.valueless_by_exception())
        return diagnostic(where, {}, "CFG_SHAPE", "shape", "Expected a selected component variant");
    return std::visit([&](const auto &selected) { return check(selected, where); }, value);
}

template<class Value>
Error check_records(std::string_view root, const std::vector<Value> &values) {
    for (std::size_t i = 0; i < values.size(); ++i)
        if (auto error = check(values[i], {root, i})) return error;
    return std::nullopt;
}

Error check_numbers(const Document &input) {
    if (auto error = check(input.engine, {"/engine"})) return error;
    const auto &parts = input.components;
    if (auto error = check_records("/components/functions", parts.functions)) return error;
    if (auto error = check_records("/components/journals", parts.journals)) return error;
    if (auto error = check_records("/components/crankshafts", parts.crankshafts)) return error;
    if (auto error = check_records("/components/pistons", parts.pistons)) return error;
    if (auto error = check_records("/components/rods", parts.rods)) return error;
    if (auto error = check_records("/components/fuels", parts.fuels)) return error;
    if (auto error = check_records("/components/throttles", parts.throttles)) return error;
    if (auto error = check_records("/components/banks", parts.banks)) return error;
    if (auto error = check_records("/components/heads", parts.heads)) return error;
    if (auto error = check_records("/components/valvetrains", parts.valvetrains)) return error;
    if (auto error = check_records("/components/camshafts", parts.camshafts)) return error;
    if (auto error = check_records("/components/intakes", parts.intakes)) return error;
    if (auto error = check_records("/components/exhausts", parts.exhausts)) return error;
    if (auto error = check_records("/components/impulses", parts.impulses)) return error;
    if (auto error = check_records("/components/ignitions", parts.ignitions)) return error;
    if (input.vehicle)
        if (auto error = check(*input.vehicle, {"/vehicle"})) return error;
    if (input.transmission) return check(*input.transmission, {"/transmission"});
    return std::nullopt;
}
}

DefaultedValues::DefaultedValues(Document input) : m_values(std::move(input)) {}

DefaultsResult normalize_values(Document input) {
    if (auto error = check_numbers(input)) return std::move(*error);
    if (!input.initialization) input.initialization.emplace();
    if (!input.vehicle) input.vehicle.emplace(default_vehicle());
    if (!input.transmission) input.transmission.emplace(default_transmission());
    if (!input.desktop) input.desktop.emplace(default_desktop());
    return DefaultedValues(std::move(input));
}

}
