#include "normalization.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {
namespace cfg = engine_sim::config_v1;
namespace detail = cfg::detail;

static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == sizeof(std::uint64_t));
static_assert(!std::is_constructible_v<detail::DefaultedValues, cfg::Document>);
static_assert(!std::is_invocable_v<decltype(detail::normalize_values), detail::DefaultedValues>);
static_assert(!std::is_assignable_v<decltype(cfg::Document::vehicle) &, std::nullptr_t>);
static_assert(!std::is_assignable_v<decltype(cfg::Engine::starter_speed) &, cfg::Measure<cfg::SpeedUnit>>);

void expect(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

std::uint64_t bits(double value) {
    std::uint64_t word;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

double from_bits(std::uint64_t word) {
    double value;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

detail::DefaultedValues normalized(cfg::Document value) {
    auto result = detail::normalize_values(std::move(value));
    if (const auto *error = std::get_if<cfg::Diagnostic>(&result))
        throw std::runtime_error(error->pointer + ": " + error->message);
    return std::get<detail::DefaultedValues>(std::move(result));
}

void check_error(cfg::Document value, std::string_view code,
                 std::string_view stage, std::string_view pointer, std::string_view field) {
    const auto result = detail::normalize_values(std::move(value));
    const auto *error = std::get_if<cfg::Diagnostic>(&result);
    expect(error && error->code == code && error->stage == stage && error->pointer == pointer,
           "Unexpected programmatic normalization diagnostic");
    expect(error->field && *error->field == field && !error->path && !error->line && !error->column &&
           !error->byte_offset, "Programmatic diagnostic lost its field or invented a source location");
}

void defaults() {
    // Frozen from the original R2 EngineSession and placed fixture at 77eb6f8.
    const auto implicit = normalized({});
    const auto &doc = implicit.values();
    expect(doc.vehicle && doc.transmission && doc.initialization && doc.desktop,
           "Normalization must fill all missing whole records");
    const auto &vehicle = *doc.vehicle;
    expect(bits(vehicle.mass.value) == UINT64_C(0x4098f40000000000), "R2 fallback mass changed");
    expect(bits(vehicle.drag_coefficient.value) == UINT64_C(0x3fd0000000000000), "R2 fallback drag changed");
    expect(bits(vehicle.cross_sectional_area.value) == UINT64_C(0x400ac18e2a4fb4cb), "R2 fallback area changed");
    expect(bits(vehicle.diff_ratio.value) == UINT64_C(0x400b5c28f5c28f5c), "R2 fallback diff changed");
    expect(bits(vehicle.tire_radius.value) == UINT64_C(0x3fd04189374bc6a8), "R2 fallback tire radius changed");
    expect(bits(vehicle.rolling_resistance.value) == UINT64_C(0x409f400000000000), "R2 fallback force changed");

    const auto &transmission = *doc.transmission;
    constexpr std::array<std::uint64_t, 6> ratios{
        UINT64_C(0x4007c28f5c28f5c3), UINT64_C(0x40008f5c28f5c28f), UINT64_C(0x3ff6e147ae147ae1),
        UINT64_C(0x3ff0000000000000), UINT64_C(0x3feae147ae147ae1), UINT64_C(0x3fe1eb851eb851ec)};
    expect(bits(transmission.max_clutch_torque.value) == UINT64_C(0x40952f45132f87ad), "R2 fallback clutch torque changed");
    expect(transmission.gears.size() == ratios.size(), "R2 fallback gear count changed");
    for (std::size_t i = 0; i < ratios.size(); ++i)
        expect(bits(transmission.gears[i].value) == ratios[i], "R2 fallback gear ordering or bits changed");

    expect(std::string_view(cfg::Initialization::profile) == "r2-piston-v1", "R2 initialization profile changed");
    expect(bits(cfg::Initialization::crankcase_pa) == UINT64_C(0x40f8bcd000000000) &&
           bits(cfg::Initialization::chamber_pa) == UINT64_C(0x40f8bcd000000000) &&
           bits(cfg::Initialization::chamber_kelvin) == UINT64_C(0x4072a26666666666),
           "R2 initial crankcase or gas assumptions changed");
    const auto &display = *doc.desktop;
    expect(!display.start_fullscreen && display.power_units == "hp" && display.torque_units == "lb-ft" &&
           display.speed_units == "mph" && display.pressure_units == "inHg" && display.boost_units == "psi",
           "R2 fallback desktop units changed");
    const std::array<std::int32_t, 11> colors{display.color_background, display.color_foreground,
        display.color_shadow, display.color_highlight1, display.color_highlight2, display.color_pink,
        display.color_red, display.color_orange, display.color_yellow, display.color_blue, display.color_green};
    constexpr std::array<std::int32_t, 11> r2_colors{0x0E1012, 0xFFFFFF, 0x0E1012, 0xEF4545, 0xFFFFFF,
        0xF394BE, 0xEE4445, 0xF4802A, 0xFDBD2E, 0x77CEE0, 0xBDD869};
    expect(colors == r2_colors, "R2 fallback palette changed");

    cfg::Document authored;
    authored.vehicle = vehicle;
    authored.vehicle->mass = {1000.0};
    authored.vehicle->drag_coefficient = {-0.0};
    authored.transmission = cfg::Transmission{{0.0}, {{-3.25}, {0.75}}};
    authored.desktop = display;
    authored.desktop->speed_units.clear();
    authored.desktop->color_shadow = -1;
    authored.initialization.emplace();
    const auto explicit_values = normalized(authored);
    const auto &explicit_doc = explicit_values.values();
    expect(explicit_doc.vehicle->mass.value == 1000.0 && std::signbit(explicit_doc.vehicle->drag_coefficient.value),
           "Explicit signed zero or vehicle values were defaulted");
    expect(explicit_doc.transmission->max_clutch_torque.value == 0.0 &&
           explicit_doc.transmission->gears.size() == 2 &&
           explicit_doc.transmission->gears[0].value == -3.25 && explicit_doc.transmission->gears[1].value == 0.75,
           "Explicit signed gearing or zero torque was replaced");
    expect(explicit_doc.initialization.has_value() && explicit_doc.desktop->speed_units.empty() &&
           explicit_doc.desktop->color_shadow == -1, "Explicit initialization or display data was replaced");

    authored.transmission->gears.clear();
    check_error(std::move(authored), "CFG_SHAPE", "shape", "/transmission/gears", "gears");
}

void unit_boundaries() {
    // Evaluated by original R2's DSL using its fixed unit and flow helpers.
    constexpr std::array<std::uint64_t, 9> original{
        UINT64_C(0x4071126666666666), UINT64_C(0x4071126666666667), UINT64_C(0x40f8bcd000000000),
        UINT64_C(0x40f77f56ac083126), UINT64_C(0x3f64dfa6340b14cd), UINT64_C(0x3f6443f0043874b6),
        UINT64_C(0x402ab41b098871d8), UINT64_C(0xc04f6a7a28bb0a2c), UINT64_C(0x400ac18e2a4fb4cb)};
    cfg::Document authored;
    authored.engine.starter_speed = {from_bits(original[7])};
    authored.components.crankshafts.resize(1);
    authored.components.crankshafts[0].tdc = {from_bits(original[6])};
    authored.components.intakes.resize(1);
    authored.components.intakes[0].intake_flow_rate = {from_bits(original[4])};
    authored.components.exhausts.resize(1);
    authored.components.exhausts[0].primary_flow_rate = {from_bits(original[5])};
    authored.components.functions.resize(2);
    auto &function = authored.components.functions[0];
    function.id = {"units"};
    function.filter_radius = {std::nextafter(1.0, 2.0)};
    for (std::size_t i = 0; i < original.size(); ++i)
        function.samples.push_back({{static_cast<double>(i)}, {from_bits(original[i])}});
    function.samples.push_back({{8.0}, {-0.0}});
    function.samples.push_back({{8.0}, {std::numeric_limits<double>::denorm_min()}});
    function.samples.push_back({{8.0}, {std::numeric_limits<double>::max()}});
    function.samples.push_back({{-0.0}, {std::nextafter(0.0, -1.0)}});
    const auto value = normalized(authored);
    const auto &doc = value.values();
    expect(bits(doc.engine.starter_speed.value) == original[7], "Legacy signed starter rad/s changed");
    expect(bits(doc.components.crankshafts[0].tdc.value) == original[6], "Unwrapped source angle changed");
    expect(bits(doc.components.intakes[0].intake_flow_rate.value) == original[4] &&
           bits(doc.components.exhausts[0].primary_flow_rate.value) == original[5], "Evaluated flow coefficients changed");
    const auto &out = doc.components.functions[0];
    expect(bits(out.filter_radius.value) == bits(function.filter_radius.value) &&
           cfg::Function::input_scale == 1 && cfg::Function::output_scale == 1,
           "Legacy interpolation radius or effective scales changed");
    expect(doc.components.functions[1].samples.empty() && out.samples.size() == function.samples.size(),
           "Empty or repeated samples were filled or discarded");
    for (std::size_t i = 0; i < function.samples.size(); ++i)
        expect(bits(out.samples[i].x.value) == bits(function.samples[i].x.value) &&
               bits(out.samples[i].y.value) == bits(function.samples[i].y.value),
               "Evaluated scalar bits or sample order changed");

    authored.components.functions[0].samples[1].y.value = std::numeric_limits<double>::quiet_NaN();
    check_error(authored, "CFG_NUMBER", "normalize", "/components/functions/0/samples/1/y", "y");
    authored.components.functions.clear();
    cfg::Camshaft cam;
    cam.lobes.push_back({std::numeric_limits<double>::infinity()});
    authored.components.camshafts = {cam};
    check_error(std::move(authored), "CFG_NUMBER", "normalize", "/components/camshafts/0/lobes/0", "lobes");
}
}

int main(int argc, char **argv) {
    try {
        if (argc != 2) throw std::invalid_argument("Expected defaults or units");
        const std::string_view test(argv[1]);
        if (test == "defaults") defaults();
        else if (test == "units") unit_boundaries();
        else throw std::invalid_argument("Unknown normalization case");
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
