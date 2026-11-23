#include "read.hpp"
#include <engine_sim/config-json.hpp>

#include "legacy_curves.h"
#include "schema.hpp"
#include "record_fields.hpp"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <unordered_map>
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

template<class T>
T decode_record(Value value) {
    T output;
    RecordFields<T>{}(output, [&](std::string_view key, auto &field) {
        using Field = std::remove_reference_t<decltype(field)>;
        if constexpr (!std::is_const_v<Field>) assign(value, key, field);
    });
    return output;
}

template<class T>
struct Decode {
    static T from(Value value) { return decode_record<T>(value); }
};

template<>
struct Decode<Function> {
    static Function from(Value value) {
        if (!value.member("kind")) return decode_record<Function>(value);
        // V2 materializes samples only after every curve and reference passes preflight.
        return Function{decode<Ref<FunctionTag>>(value.member("id").value()), {}, {}};
    }
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

using Harmonic = legacy_curves::HarmonicCamLobe;
using DefaultCurve = legacy_curves::DefaultTurbulence;
struct PortScale {
    double lift = 1.0;
    double flow = 1.0;
    std::optional<Value> definition;
    std::size_t index = 0;
};

struct PortTable {
    Value definition;
    Value points;
    std::size_t index;
    std::size_t count;
    double radius;
    double max_lift;
    double max_flow;
    double lift_unit;
};

struct PortFlowPlan {
    PortTable table;
    PortScale scale;
};

enum class PointStyle { ignition, cam, fuel };

struct AuthoredPoints {
    Value values;
    PointStyle style;
};

using CurvePlan = std::variant<Value, Harmonic, DefaultCurve, PortFlowPlan, AuthoredPoints>;

struct AuthoringPlan {
    NormalizedDocument skeleton;
    std::vector<CurvePlan> curves;
};

Diagnostic authoring_error(Value record, std::string_view collection, std::size_t index, std::string_view field,
                           std::string_view message, const char *code) {
    Diagnostic problem;
    problem.code = code;
    const auto error_code = std::string_view(code);
    problem.stage = error_code == "CFG_REF" || error_code == "CFG_ID" ? "graph" : "normalize";
    problem.pointer = std::string(collection) + '/' + std::to_string(index) + '/' + std::string(field);
    problem.field = field;
    problem.component_id = record.member("id").value().text();
    problem.message = message;
    if (const auto value = record.member(field)) set_source_location(problem, record.json.bytes, value->node().span.begin);
    return problem;
}

Diagnostic curve_error(Value record, std::size_t index, std::string_view field,
                       std::string_view message, const char *code = "CFG_CURVE") {
    return authoring_error(record, "/components/functions", index, field, message, code);
}

double real_or(Value record, std::string_view key, double fallback) {
    const auto value = record.member(key);
    return value ? value->node().real : fallback;
}

std::size_t array_size(Value array) {
    std::size_t count = 0;
    for (auto i = array.index + 1; i < array.node().after; i = array.json.nodes[i].after) ++count;
    return count;
}

template<class Definition, class Read>
std::optional<Diagnostic> read_registry(Value root, std::string_view field,
                                        std::unordered_map<std::string_view, Definition> &registry, Read read) {
    const auto array = root.member(field);
    if (!array) return {};
    std::size_t index = 0;
    for (auto i = array->index + 1; i < array->node().after; i = root.json.nodes[i].after, ++index) {
        const Value record{root.json, i};
        const auto id = record.member("id").value();
        const auto &key = id.text();
        if (!registry.emplace(key, read(record, index)).second)
            return authoring_error(record, "/" + std::string(field), index, "id",
                                   "Duplicate authoring definition ID", "CFG_ID");
    }
    return {};
}

PortTable read_port_table(Value record, std::size_t index) {
    const auto points = record.member("points").value();
    const auto authored_unit = record.member("lift_unit")->text();
    const double unit = authored_unit == "thou" ? units::thou : authored_unit == "mm" ? units::mm : units::m;
    PortTable table{record, points, index, 0,
                    units::distance(record.member("filter_radius")->node().real, unit), 0, 0, unit};
    for (auto i = points.index + 1; i < points.node().after; i = points.json.nodes[i].after) {
        const Value point{points.json, i};
        ++table.count;
        table.max_lift = std::max(table.max_lift, point.member("lift")->node().real);
        table.max_flow = std::max(table.max_flow, point.member("flow_scfm")->node().real);
    }
    return table;
}

std::variant<Harmonic, Diagnostic> plan_harmonic(Value record, std::size_t index) {
    const auto steps = record.member("steps");
    const auto lift_mm = record.member("max_lift_mm");
    if (lift_mm && record.member("max_lift_thou"))
        return curve_error(record, index, "max_lift_mm", "Use one lift unit for the harmonic profile");
    const double lift = lift_mm ? units::distance(lift_mm->node().real, units::mm)
                                : units::distance(real_or(record, "max_lift_thou", 300.0), units::thou);
    const auto lift_field = lift_mm ? "max_lift_mm" : "max_lift_thou";
    if (lift <= units::distance(50, units::thou))
        return curve_error(record, index, lift_field, "Lift must be greater than the 50-thou reference height");
    const auto lobe = legacy_curves::harmonic_cam_lobe({
        units::angle(record.member("duration_at_50_thou_deg")->node().real, units::deg),
        real_or(record, "gamma", 1.0), lift,
        steps ? integer32(record.json, steps->node()).value : 100
    });
    if (!std::isfinite(lobe.cosine_at_50_thou) || lobe.cosine_at_50_thou >= 1.0)
        return curve_error(record, index, record.member("gamma") ? "gamma" : lift_field,
                           "The lift and gamma must admit a nonzero harmonic angle at 50 thou");
    if (lobe.frequency <= 0 || lobe.step <= 0 || !std::isfinite(lobe.frequency) ||
        !std::isfinite(lobe.step * (lobe.input.steps - 1)))
        return curve_error(record, index, "duration_at_50_thou_deg",
                           "Duration must produce finite, nonzero harmonic spacing and extent");
    return lobe;
}

std::optional<Diagnostic> check_fuel(const AuthoredPoints &table, Value profile, std::size_t index) {
    const auto points = table.values;
    std::size_t row = 0;
    for (auto i = points.index + 1; i < points.node().after; i = points.json.nodes[i].after, ++row) {
        const Value point{points.json, i};
        const auto direct = point.member("flame_speed");
        const auto multiplier = point.member("flame_speed_multiplier");
        const auto fail = [&](std::string_view field, std::string_view message) {
            auto problem = curve_error(profile, index, "points", message);
            problem.pointer += "/" + std::to_string(row) + "/" + std::string(field);
            problem.field = field;
            locate_diagnostic(points.json, problem);
            return problem;
        };
        if (direct.has_value() == multiplier.has_value())
            return fail(direct ? "flame_speed_multiplier" : "flame_speed",
                        "Supply exactly one flame speed or flame speed multiplier");
        const double factor = direct ? direct->node().real : multiplier->node().real;
        const double value = real_or(point, "scale", 1.0) * factor;
        if (!std::isfinite(value) || (!direct && !std::isfinite(value * point.member("turbulence_speed")->node().real)))
            return fail(direct ? "flame_speed" : "flame_speed_multiplier", "Scaled flame speed is outside the finite range");
    }
    return {};
}

Sample authored_point(PointStyle style, Value point) {
    const auto value = [&](std::string_view field) { return point.member(field)->node().real; };
    if (style == PointStyle::ignition)
        return {{units::rpm(value("rpm"))}, {units::angle(value("advance_deg"), units::deg)}};
    if (style == PointStyle::cam)
        return {{units::angle(value("angle_deg"), units::deg)}, {value("lift_m")}};
    const auto turbulence = value("turbulence_speed");
    const auto scale = real_or(point, "scale", 1.0);
    if (const auto speed = point.member("flame_speed")) return {{turbulence}, {scale * speed->node().real}};
    return {{turbulence}, {(scale * value("flame_speed_multiplier")) * turbulence}};
}

std::variant<AuthoringPlan, Diagnostic> prepare_authoring(const ParsedJson &parsed) {
    const Value root{parsed, 0};
    std::unordered_map<std::string_view, PortTable> tables;
    if (auto problem = read_registry(root, "port_flow_tables", tables, read_port_table)) return std::move(*problem);
    std::unordered_map<std::string_view, PortScale> scales;
    if (auto problem = read_registry(root, "port_flow_scales", scales, [](Value record, std::size_t index) {
            return PortScale{real_or(record, "lift_scale", 1.0), real_or(record, "flow_attenuation", 1.0), record, index};
        })) return std::move(*problem);

    auto document = decode<Document>(root);
    const auto list = root.member("components")->member("functions").value();
    std::vector<CurvePlan> plans;
    plans.reserve(document.components.functions.size());
    constexpr std::size_t sample_limit = 1'048'576;
    std::size_t sample_count = 0;
    for (auto child = list.index + 1; child < list.node().after; child = parsed.nodes[child].after) {
        const auto index = plans.size();
        const Value record{parsed, child};
        const auto kind = record.member("kind").value().text();
        auto &profile = document.components.functions[index];
        std::size_t count = 0;
        std::string_view limit_field = "kind";
        if (kind == "samples") {
            const auto samples = record.member("samples").value();
            count = array_size(samples);
            profile.filter_radius = {record.member("filter_radius")->node().real};
            if (count >= 2 && profile.filter_radius.value <= 0)
                return curve_error(record, index, "filter_radius", "Multivalued curves require a positive filter radius");
            plans.emplace_back(samples);
            limit_field = "samples";
        } else if (kind == "harmonic_cam_lobe") {
            auto plan = plan_harmonic(record, index);
            if (auto *problem = std::get_if<Diagnostic>(&plan)) return std::move(*problem);
            const auto lobe = std::get<Harmonic>(plan);
            profile.filter_radius = {lobe.step};
            count = 2 * static_cast<std::size_t>(lobe.input.steps) - 1;
            plans.emplace_back(lobe);
            limit_field = "steps";
        } else if (kind == "port_flow_28_inh2o") {
            const auto table = tables.find(record.member("table")->text());
            if (table == tables.end())
                return curve_error(record, index, "table", "Missing port flow table", "CFG_REF");
            const PortScale defaults;
            const PortScale *values = &defaults;
            if (const auto scale_id = record.member("scale")) {
                const auto scale = scales.find(scale_id->text());
                if (scale == scales.end())
                    return curve_error(record, index, "scale", "Missing port flow scale", "CFG_REF");
                values = &scale->second;
            }
            const auto &data = table->second;
            const double lift = data.max_lift * values->lift;
            const double flow = data.max_flow * values->flow;
            if (data.radius <= 0)
                return authoring_error(data.definition, "/port_flow_tables", data.index, "filter_radius",
                                       "The filter radius is too small in metres", "CFG_CURVE");
            const bool lift_overflows = !std::isfinite(lift);
            if (lift_overflows || !std::isfinite(flow) || !std::isfinite(legacy_curves::port_flow_28_inh2o(flow))) {
                constexpr auto message = "Port flow scaling exceeds the finite measurement range";
                if (values->definition)
                    return authoring_error(*values->definition, "/port_flow_scales", values->index,
                                           lift_overflows ? "lift_scale" : "flow_attenuation", message, "CFG_CURVE");
                return authoring_error(data.definition, "/port_flow_tables", data.index, "points", message, "CFG_CURVE");
            }
            profile.filter_radius = {data.radius};
            count = data.count;
            plans.emplace_back(PortFlowPlan{data, *values});
            limit_field = "table";
        } else if (kind == "ignition_timing" || kind == "cam_samples" || kind == "fuel_flame_speed") {
            const auto style = kind == "ignition_timing" ? PointStyle::ignition
                : kind == "cam_samples" ? PointStyle::cam : PointStyle::fuel;
            const auto points = record.member("points").value();
            AuthoredPoints grid{points, style};
            std::string_view radius = "filter_radius";
            if (style == PointStyle::ignition) radius = "filter_radius_rpm";
            if (style == PointStyle::cam) radius = "filter_radius_deg";
            const auto raw_radius = record.member(radius)->node().real;
            const auto effective_radius = style == PointStyle::ignition ? units::rpm(raw_radius)
                : style == PointStyle::cam ? units::angle(raw_radius, units::deg) : raw_radius;
            if (effective_radius <= 0)
                return curve_error(record, index, radius, "Filter radius is too small in runtime units");
            if (style == PointStyle::fuel)
                if (auto problem = check_fuel(grid, record, index)) return std::move(*problem);
            profile.filter_radius = {effective_radius};
            count = array_size(points);
            plans.emplace_back(grid);
            limit_field = "points";
        } else {
            profile.filter_radius = {DefaultCurve::radius};
            count = DefaultCurve::samples;
            plans.emplace_back(DefaultCurve{});
        }
        if (count > sample_limit - sample_count)
            return curve_error(record, index, limit_field, "Too many curve samples after expansion", "CFG_LIMIT");
        sample_count += count;
    }

    auto skeleton = normalize(std::move(document));
    if (auto *problem = std::get_if<Diagnostic>(&skeleton)) {
        locate_diagnostic(parsed, *problem);
        return std::move(*problem);
    }
    return AuthoringPlan{std::get<NormalizedDocument>(std::move(skeleton)), std::move(plans)};
}

ModelResult expand(const ParsedJson &parsed, AuthoringPlan plan) {
    auto model = plan.skeleton.values();
    for (std::size_t i = 0; i < plan.curves.size(); ++i) {
        auto &samples = model.components.functions[i].samples;
        if (const auto *input = std::get_if<Value>(&plan.curves[i])) {
            samples = decode<std::vector<Sample>>(*input);
        } else {
            const auto add = [&](double x, double y) { samples.push_back({{x}, {y}}); };
            if (const auto *lobe = std::get_if<Harmonic>(&plan.curves[i])) {
                samples.reserve(2 * static_cast<std::size_t>(lobe->input.steps) - 1);
                lobe->sample(add);
            } else if (const auto *flow = std::get_if<PortFlowPlan>(&plan.curves[i])) {
                const auto points = flow->table.points;
                samples.reserve(flow->table.count);
                for (auto j = points.index + 1; j < points.node().after; j = points.json.nodes[j].after) {
                    const Value point{points.json, j};
                    const auto lift = point.member("lift")->node().real * flow->scale.lift;
                    const auto scfm = point.member("flow_scfm")->node().real * flow->scale.flow;
                    add(units::distance(lift, flow->table.lift_unit), legacy_curves::port_flow_28_inh2o(scfm));
                }
            } else if (const auto *grid = std::get_if<AuthoredPoints>(&plan.curves[i])) {
                const auto array = grid->values;
                samples.reserve(array_size(array));
                for (auto j = array.index + 1; j < array.node().after; j = array.json.nodes[j].after)
                    samples.push_back(authored_point(grid->style, {array.json, j}));
            } else {
                samples.reserve(DefaultCurve::samples);
                DefaultCurve::sample(add);
            }
        }
    }
    auto result = normalize(std::move(model));
    if (auto *problem = std::get_if<Diagnostic>(&result)) locate_diagnostic(parsed, *problem);
    return result;
}

ModelResult normalize_v1(const ParsedJson &parsed) {
    auto model = normalize(decode<Document>({parsed, 0}));
    if (auto *problem = std::get_if<Diagnostic>(&model)) locate_diagnostic(parsed, *problem);
    return model;
}

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
    return normalize_v1(parsed);
}

}

namespace engine_sim::config_json {
namespace {
using namespace config_v1;
using namespace config_v1::json_detail;

template<class Result, class ProcessV1, class ProcessV2>
Result read_versioned(std::string_view input, ProcessV1 v1, ProcessV2 v2) {
    auto result = parse(input);
    if (auto *error = std::get_if<ParseError>(&result)) return std::move(error->diagnostic);
    auto &parsed = std::get<ParsedJson>(result);
    int format = 0;
    if (auto problem = validate_document(parsed, format, true)) return std::move(*problem);
    if (format == 1) return v1(normalize_v1(parsed));
    auto plan = prepare_authoring(parsed);
    if (auto *problem = std::get_if<Diagnostic>(&plan)) return std::move(*problem);
    return v2(parsed, std::get<AuthoringPlan>(std::move(plan)));
}
}

config_v1::ModelResult read_model(std::string_view bytes) {
    return read_versioned<ModelResult>(bytes, [](ModelResult model) { return model; }, expand);
}

RewriteResult rewrite(std::string_view bytes) {
    return read_versioned<RewriteResult>(bytes, [](ModelResult model) -> RewriteResult {
        if (auto *problem = std::get_if<Diagnostic>(&model)) return std::move(*problem);
        return write_json(std::get<NormalizedDocument>(model));
    }, [](ParsedJson &parsed, const AuthoringPlan &) -> RewriteResult {
        return std::move(parsed.bytes);
    });
}
}
