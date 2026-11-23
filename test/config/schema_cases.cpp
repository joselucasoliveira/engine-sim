#include "read.hpp"
#include "schema.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
namespace cfg = engine_sim::config_v1;
namespace json = engine_sim::config_v1::json_detail;

void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

std::string read(const char *path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error(std::string("Cannot open ") + path);
    const auto size = stream.tellg();
    if (size < 0 || size > static_cast<std::streamoff>(json::document_bytes))
        throw std::runtime_error("Unexpected fixture extent");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    stream.seekg(0);
    if (!stream.read(bytes.data(), static_cast<std::streamsize>(size)))
        throw std::runtime_error("Cannot read the full fixture");
    return bytes;
}

std::string replace(std::string text, std::string_view old, std::string_view value) {
    const auto where = text.find(old);
    require(where != std::string::npos, "Fixture field required for mutation is missing");
    text.replace(where, old.size(), value);
    return text;
}

template<class Result>
const cfg::Diagnostic &problem(const Result &result, std::string_view code, std::string_view stage,
                              std::string_view pointer) {
    require(std::holds_alternative<cfg::Diagnostic>(result), "Invalid engine document was accepted");
    const auto &diagnostic = std::get<cfg::Diagnostic>(result);
    if (diagnostic.code != code || diagnostic.stage != stage || diagnostic.pointer != pointer)
        throw std::runtime_error(diagnostic.code + " " + diagnostic.stage + " " +
                                 diagnostic.pointer + " " + diagnostic.message);
    return diagnostic;
}

cfg::NormalizedDocument normalized(std::string_view bytes) {
    auto result = json::read_normalized(bytes);
    if (const auto *diagnostic = std::get_if<cfg::Diagnostic>(&result))
        throw std::runtime_error(diagnostic->code + " " + diagnostic->pointer + " " + diagnostic->message);
    return std::get<cfg::NormalizedDocument>(std::move(result));
}

void overlapping_rules() {
    json::Rules rules(3);
    rules[0].alternatives = {1, 2};
    rules[1].type = json::Type::integer;
    rules[2].minimum = "2";
    for (const auto &[token, expected] : {std::pair{"1", true}, {"2", false}, {"2.5", true}, {"1.5", false}}) {
        const auto tape = json::parse(std::string("{\"value\":") + token + '}');
        const auto &bytes = std::get<json::ParsedJson>(tape);
        require(!json::validate_shape(rules, {bytes, 1}) == expected,
                "oneOf did not enforce exactly one arm");
    }
}

void parity(const std::string &fixture) {
    const auto authored = json::read_authored(fixture);
    auto authored_model = std::get<cfg::Document>(authored);
    require(!authored_model.initialization && !authored_model.vehicle && !authored_model.transmission &&
            !authored_model.desktop, "Optional records were materialized during structural intake");
    const auto direct = std::get<cfg::NormalizedDocument>(cfg::normalize(std::move(authored_model)));
    const auto with_defaults = normalized(fixture);
    const auto &from_json = with_defaults.values();
    const auto &from_model = direct.values();
    require(from_json.initialization && from_json.vehicle && from_json.transmission && from_json.desktop &&
            from_model.initialization && from_model.vehicle && from_model.transmission && from_model.desktop,
            "Defaults were not materialized in both normalized routes");
    require(from_json.vehicle->mass.value == from_model.vehicle->mass.value &&
            from_json.vehicle->diff_ratio.value == from_model.vehicle->diff_ratio.value &&
            from_json.transmission->gears.size() == from_model.transmission->gears.size() &&
            from_json.desktop->color_blue == from_model.desktop->color_blue &&
            from_json.desktop->start_fullscreen == from_model.desktop->start_fullscreen &&
            with_defaults.topology().cylinders.size() == 1 &&
            with_defaults.topology().cylinders.size() == direct.topology().cylinders.size(),
            "Normalized JSON defaults differ from direct model defaults");
    require(from_json.components.impulses.at(0).path == "impulse.wav",
            "Resource existence incorrectly affected schema and graph intake");
    require(from_json.components.crankshafts.at(0).crank_throw.value == 0.04 &&
            from_json.engine.redline.value == 628.32, "Typed quantity mapping differs");
    normalized(replace(fixture, "\"format_version\": 1", "\"format_version\": 10e-1"));
    const auto sign = normalized(replace(fixture, "\"jitter\": 0", "\"jitter\": -0"));
    require(std::signbit(sign.values().engine.jitter.value), "Negative zero changed during typed decoding");

    const auto future = json::read_authored(R"({"format_version":4294967296})");
    problem(future, "CFG_VERSION", "version", "/format_version");
    problem(json::read_authored("{}"), "CFG_SHAPE", "shape", "/format_version");
    problem(json::read_authored(R"({"format_version":null})"), "CFG_SHAPE", "shape", "/format_version");
    problem(json::read_authored(R"({"format_version":1.00000000000000000000001})"), "CFG_SHAPE", "shape", "/format_version");
    problem(json::read_authored(R"({"format_version":1/* comment */})"), "CFG_SYNTAX", "parse", "/format_version");
    problem(json::read_authored(replace(fixture, "\"name\":", "\"typo/~\":")),
            "CFG_SHAPE", "shape", "/engine/name");
    auto unknown = replace(fixture, "\"redline\":", "\"typo/~\":0,\"redline\":");
    const auto unknown_result = json::read_authored(unknown);
    const auto &unknown_issue = problem(unknown_result, "CFG_SHAPE", "shape", "/engine/typo~1~0");
    require(unknown_issue.byte_offset == unknown.find("\"typo/~\"") && unknown_issue.line == 7 &&
            unknown_issue.column == 5, "Unexpected property location differs");
    const auto ref = replace(fixture, "\"fuel\": \"gas\"", "\"fuel\": \"missing\"");
    require(std::holds_alternative<cfg::Document>(json::read_authored(ref)), "Structural reader resolved a domain ID");
    const auto ref_result = json::read_normalized(ref);
    const auto &ref_issue = problem(ref_result, "CFG_REF", "graph", "/engine/fuel");
    require(ref_issue.byte_offset == ref.find("\"missing\""), "Reference diagnostic has the wrong source span");
    problem(json::read_normalized(replace(fixture, "\"fuel\": \"gas\"", "\"fuel\": \"2wrong\"")),
            "CFG_REF", "graph", "/engine/fuel");
    problem(json::read_authored(replace(fixture, "\"input_scale\": 1", "\"input_scale\": 1.00000000000000000000001")),
            "CFG_SHAPE", "shape", "/components/functions/0/input_scale");
    problem(json::read_authored(replace(fixture, "\"engine\":", "\"vehicle\":null,\"engine\":")),
            "CFG_SHAPE", "shape", "/vehicle");
    problem(json::read_authored(replace(fixture, "\"engine\":", "\"vehicle\":{},\"engine\":")),
            "CFG_SHAPE", "shape", "/vehicle/mass");

    problem(json::read_authored(replace(fixture, "\"kind\": \"direct\"", "\"kind\": \"invalid\"")),
            "CFG_SHAPE", "shape", "/components/throttles/0/kind");
    problem(json::read_authored(replace(fixture, "\"kind\": \"direct\"", "\"kind\": \"direct\", \"min_v\": 0")),
            "CFG_SHAPE", "shape", "/components/throttles/0/min_v");
    const auto governor = replace(fixture, "\"kind\": \"direct\"",
        "\"kind\": \"governor\", \"min_speed\": 62, \"max_speed\": 628, \"min_v\": 0, \"max_v\": 1, \"k_s\": 1, \"k_d\": 1");
    const auto governed = normalized(governor);
    require(std::holds_alternative<cfg::Governor>(governed.values().components.throttles.at(0)),
            "Governor variant did not decode");
    const auto vtec = replace(fixture, "\"kind\": \"standard\"",
        "\"kind\": \"vtec\", \"vtec_intake_camshaft\": \"ic\", \"vtec_exhaust_camshaft\": \"ec\", "
        "\"min_rpm\": 250, \"min_speed\": 0, \"manifold_vacuum\": 0, \"min_throttle_position\": 0");
    const auto valve = normalized(vtec);
    require(std::holds_alternative<cfg::Vtec>(valve.values().components.valvetrains.at(0)),
            "VTEC variant did not decode");
    overlapping_rules();
}
}

int main(int argc, char **argv) {
    try {
        if (argc != 3) return 2;
        const std::string_view mode = argv[1];
        const auto input = read(argv[2]);
        if (mode == "parity") parity(input);
        else if (mode == "embedding") {
            require(input == json::schema_bytes(), "Embedded schema bytes differ from the distributed schema");
            std::cout << json::schema_digest() << '\n';
        }
        else if (mode == "structural") {
            const auto result = json::read_authored(input);
            if (const auto *diagnostic = std::get_if<cfg::Diagnostic>(&result)) {
                std::cout << diagnostic->code << '\n' << diagnostic->stage << '\n' << diagnostic->pointer << '\n';
                return 1;
            }
        }
        else return 2;
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
