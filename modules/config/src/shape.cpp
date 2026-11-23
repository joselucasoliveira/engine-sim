#include "validation_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace engine_sim::config_v1::detail {
[[noreturn]] void reject(const Cursor &where, std::string_view code,
                         std::string_view stage, std::string_view message) {
    Diagnostic problem;
    problem.code = code;
    problem.stage = stage;
    problem.pointer = where.pointer;
    problem.message = message;
    if (!where.member.empty()) problem.field = where.member;
    if (!where.component.empty() && where.component.size() <= 64 &&
        std::all_of(where.component.begin(), where.component.end(),
                    [](unsigned char c) { return c >= 0x21 && c < 0x7f; }))
        problem.component_id = where.component;
    throw InvalidModel{std::move(problem)};
}

void check_text(const Cursor &where, std::string_view value, bool required, std::size_t max_scalars) {
    if (required && value.empty())
        reject(where, "CFG_SHAPE", "shape", "Expected a nonempty string");
    std::size_t count = 0;
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i++]);
        unsigned rest = 0;
        std::uint32_t scalar = first;
        if (first >= 0xc2 && first <= 0xdf) { rest = 1; scalar = first & 0x1f; }
        else if (first >= 0xe0 && first <= 0xef) { rest = 2; scalar = first & 0x0f; }
        else if (first >= 0xf0 && first <= 0xf4) { rest = 3; scalar = first & 0x07; }
        else if (first == 0 || first >= 0x80)
            reject(where, "CFG_UTF8", "normalize", "Expected a UTF-8 string without NUL");
        if (rest > value.size() - i)
            reject(where, "CFG_UTF8", "normalize", "Truncated UTF-8 sequence");
        const unsigned length = rest;
        while (rest > 0) {
            const auto tail = static_cast<unsigned char>(value[i++]);
            if ((tail & 0xc0) != 0x80)
                reject(where, "CFG_UTF8", "normalize", "Invalid UTF-8 continuation byte");
            scalar = (scalar << 6) | (tail & 0x3f);
            --rest;
        }
        if ((length == 1 && scalar < 0x80) || (length == 2 && scalar < 0x800) ||
            (length == 3 && scalar < 0x10000) || (scalar >= 0xd800 && scalar <= 0xdfff) ||
            scalar > 0x10ffff)
            reject(where, "CFG_UTF8", "normalize", "Expected a Unicode scalar value in shortest form");
        if (++count > max_scalars)
            reject(where, "CFG_LIMIT", "normalize", "String exceeds the configured scalar limit");
    }
}

void check_id(const Cursor &where, std::string_view value) {
    check_text(where, value, false, 64);
    const auto letter = [](char c) { return ('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z'); };
    if (value.empty() || !letter(value.front()))
        reject(where, "CFG_REF", "graph", "Expected an ASCII identifier beginning with a letter");
    for (char c : value) {
        if (!letter(c) && !(c >= '0' && c <= '9') && c != '.' && c != '_' && c != '-')
            reject(where, "CFG_REF", "graph", "Character is outside the identifier grammar");
    }
}

namespace {
void require_count(const Cursor &where, std::size_t count, std::size_t min, std::size_t max) {
    if (count < min) reject(where, "CFG_SHAPE", "shape", "Required array is empty");
    if (count > max) reject(where, "CFG_LIMIT", "normalize", "Array exceeds the configured extent");
}

void lower_bound(const Cursor &where, double value, double limit, bool exclusive) {
    // The fixed-unit normalization pass owns nonfinite diagnostics.
    if (std::isfinite(value) && (exclusive ? value <= limit : value < limit))
        reject(where, "CFG_SHAPE", "shape", "Value is below its published lower bound");
}

void shape(const Function &value, const Cursor &where) {
    require_count(where.field("samples"), value.samples.size(), 0, curve_sample_limit);
}

void shape(const Crankshaft &value, const Cursor &where) {
    lower_bound(where.field("throw"), value.crank_throw.value, 0.0, true);
    lower_bound(where.field("flywheel_mass"), value.flywheel_mass.value, 0.0, false);
    lower_bound(where.field("mass"), value.mass.value, 0.0, false);
    lower_bound(where.field("friction_torque"), value.friction_torque.value, 0.0, false);
    lower_bound(where.field("moment_of_inertia"), value.moment_of_inertia.value, 0.0, true);
    require_count(where.field("journals"), value.journals.size(), 0, definition_limit);
}

void shape(const Piston &value, const Cursor &where) {
    lower_bound(where.field("mass"), value.mass.value, 0.0, true);
}

void shape(const ConnectingRod &value, const Cursor &where) {
    lower_bound(where.field("mass"), value.mass.value, 0.0, true);
    lower_bound(where.field("moment_of_inertia"), value.moment_of_inertia.value, 0.0, true);
    lower_bound(where.field("length"), value.length.value, 0.0, true);
    require_count(where.field("journals"), value.journals.size(), 0, definition_limit);
}

void shape(const Fuel &value, const Cursor &where) {
    check_text(where.field("name"), value.name);
    lower_bound(where.field("molecular_mass"), value.molecular_mass.value, 0.0, true);
    lower_bound(where.field("energy_density"), value.energy_density.value, 0.0, false);
    lower_bound(where.field("density"), value.density.value, 0.0, true);
    lower_bound(where.field("molecular_afr"), value.molecular_afr.value, 0.0, true);
}

void shape(const DirectThrottle &value, const Cursor &where) {
    lower_bound(where.field("gamma"), value.gamma.value, 0.0, true);
}

void shape(const Governor &value, const Cursor &where) {
    lower_bound(where.field("min_speed"), value.min_speed.value, 0.0, false);
    lower_bound(where.field("k_s"), value.k_s.value, 0.0, false);
    lower_bound(where.field("k_d"), value.k_d.value, 0.0, false);
    lower_bound(where.field("gamma"), value.gamma.value, 0.0, true);
}

void shape(const Engine &value, const Cursor &where) {
    check_text(where.field("name"), value.name);
    lower_bound(where.field("redline"), value.redline.value, 0.0, true);
    lower_bound(where.field("starter_torque"), value.starter_torque.value, 0.0, false);
    lower_bound(where.field("simulation_frequency"), value.simulation_frequency.value, 0.0, true);
    require_count(where.field("crankshafts"), value.crankshafts.size(), 1, definition_limit);
    require_count(where.field("banks"), value.banks.size(), 1, definition_limit);
}

void shape(const CylinderBank &value, const Cursor &where) {
    lower_bound(where.field("bore"), value.bore.value, 0.0, true);
    require_count(where.field("cylinders"), value.cylinders.size(), 1, cylinder_limit);
}

void shape(const Head &value, const Cursor &where) {
    lower_bound(where.field("chamber_volume"), value.chamber_volume.value, 0.0, true);
    lower_bound(where.field("intake_runner_volume"), value.intake_runner_volume.value, 0.0, true);
    lower_bound(where.field("intake_runner_cross_section_area"), value.intake_runner_cross_section_area.value, 0.0, true);
    lower_bound(where.field("exhaust_runner_volume"), value.exhaust_runner_volume.value, 0.0, true);
    lower_bound(where.field("exhaust_runner_cross_section_area"), value.exhaust_runner_cross_section_area.value, 0.0, true);
}

void shape(const Camshaft &value, const Cursor &where) {
    lower_bound(where.field("base_radius"), value.base_radius.value, 0.0, false);
    require_count(where.field("lobes"), value.lobes.size(), 1, definition_limit);
}

void shape(const Intake &value, const Cursor &where) {
    lower_bound(where.field("plenum_volume"), value.plenum_volume.value, 0.0, true);
    lower_bound(where.field("plenum_cross_section_area"), value.plenum_cross_section_area.value, 0.0, true);
    lower_bound(where.field("molecular_afr"), value.molecular_afr.value, 0.0, true);
}

void shape(const ExhaustSystem &value, const Cursor &where) {
    lower_bound(where.field("length"), value.length.value, 0.0, true);
    lower_bound(where.field("collector_cross_section_area"), value.collector_cross_section_area.value, 0.0, true);
}

void shape(const ImpulseResponse &value, const Cursor &where) {
    check_text(where.field("path"), value.path, true);
}

void shape(const Ignition &value, const Cursor &where) {
    lower_bound(where.field("rev_limit"), value.rev_limit.value, 0.0, true);
    lower_bound(where.field("limiter_duration"), value.limiter_duration.value, 0.0, false);
    require_count(where.field("posts"), value.posts.size(), 0, definition_limit);
}

void shape(const Vehicle &value, const Cursor &where) {
    lower_bound(where.field("mass"), value.mass.value, 0.0, true);
    lower_bound(where.field("tire_radius"), value.tire_radius.value, 0.0, true);
}

void shape(const Transmission &value, const Cursor &where) {
    lower_bound(where.field("max_clutch_torque"), value.max_clutch_torque.value, 0.0, false);
    require_count(where.field("gears"), value.gears.size(), 1, definition_limit);
}

void shape(const Desktop &value, const Cursor &where) {
    check_text(where.field("power_units"), value.power_units);
    check_text(where.field("torque_units"), value.torque_units);
    check_text(where.field("speed_units"), value.speed_units);
    check_text(where.field("pressure_units"), value.pressure_units);
    check_text(where.field("boost_units"), value.boost_units);
}

template<class Value>
void shape(const Value &, const Cursor &) {}

template<class... Alternatives>
void shape(const std::variant<Alternatives...> &value, const Cursor &where) {
    if (value.valueless_by_exception())
        reject(where, "CFG_SHAPE", "shape", "Expected a selected component alternative");
    std::visit([&](const auto &alternative) { shape(alternative, where); }, value);
}

template<class Value>
std::string_view id(const Value &value) { return value.id.value; }

template<class... Alternatives>
std::string_view id(const std::variant<Alternatives...> &value) {
    return value.valueless_by_exception() ? std::string_view{} :
        std::visit([](const auto &alternative) { return id(alternative); }, value);
}

template<class Value>
void check_table(const std::vector<Value> &values, const Cursor &where, std::size_t &definition_count) {
    require_count(where, values.size(), 1, definition_limit);
    if (values.size() > definition_limit - definition_count)
        reject(where, "CFG_LIMIT", "normalize", "Too many component definitions in the document");
    definition_count += values.size();
    for (std::size_t i = 0; i < values.size(); ++i)
        shape(values[i], where.item(i).owner(id(values[i])));
}
}

void check_shape(const Document &input) {
    shape(input.engine, Cursor{"/engine"}.owner(input.engine.id.value));
    const auto &parts = input.components;
    std::size_t definition_count = 1;
    const Cursor components{"/components"};
    check_table(parts.functions, components.field("functions"), definition_count);
    check_table(parts.journals, components.field("journals"), definition_count);
    check_table(parts.crankshafts, components.field("crankshafts"), definition_count);
    check_table(parts.pistons, components.field("pistons"), definition_count);
    check_table(parts.rods, components.field("rods"), definition_count);
    check_table(parts.fuels, components.field("fuels"), definition_count);
    check_table(parts.throttles, components.field("throttles"), definition_count);
    check_table(parts.banks, components.field("banks"), definition_count);
    check_table(parts.heads, components.field("heads"), definition_count);
    check_table(parts.valvetrains, components.field("valvetrains"), definition_count);
    check_table(parts.camshafts, components.field("camshafts"), definition_count);
    check_table(parts.intakes, components.field("intakes"), definition_count);
    check_table(parts.exhausts, components.field("exhausts"), definition_count);
    check_table(parts.impulses, components.field("impulses"), definition_count);
    check_table(parts.wires, components.field("wires"), definition_count);
    check_table(parts.ignitions, components.field("ignitions"), definition_count);
    std::size_t samples = 0;
    for (std::size_t i = 0; i < parts.functions.size(); ++i) {
        const auto size = parts.functions[i].samples.size();
        if (size > curve_sample_limit - samples)
            reject(components.field("functions").item(i).owner(parts.functions[i].id.value).field("samples"),
                   "CFG_LIMIT", "normalize", "Too many curve samples in the document");
        samples += size;
    }
    if (input.vehicle) shape(*input.vehicle, {"/vehicle"});
    if (input.transmission) shape(*input.transmission, {"/transmission"});
    if (input.desktop) shape(*input.desktop, {"/desktop"});
}

}
