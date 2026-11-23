#include "lexical.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
namespace json = engine_sim::config_v1::json_detail;
using json::IntegerStatus;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

json::ParsedJson accept(std::string_view input) {
    auto result = json::parse(input);
    if (const auto *error = std::get_if<json::ParseError>(&result))
        throw std::runtime_error(error->diagnostic.code + ": " + error->diagnostic.message);
    return std::get<json::ParsedJson>(std::move(result));
}

json::ParseError reject(std::string_view input, std::string_view expected,
                        std::size_t offset, std::string_view pointer = {}) {
    auto result = json::parse(input);
    const auto *error = std::get_if<json::ParseError>(&result);
    require(error != nullptr, "Malformed or excessive input was accepted");
    const auto &diagnostic = error->diagnostic;
    if (diagnostic.code != expected || diagnostic.stage != "parse" ||
        diagnostic.byte_offset != offset || diagnostic.pointer != pointer)
        throw std::runtime_error("Unexpected diagnostic " + diagnostic.code + " at byte " +
                                 std::to_string(diagnostic.byte_offset.value_or(input.size())) +
                                 " at pointer " + diagnostic.pointer);
    return *error;
}

std::string field(std::string_view token) { return "{\"n\":" + std::string(token) + '}'; }

void real(std::string_view token, double expected) {
    const auto parsed = accept(field(token));
    const auto &node = parsed.nodes.at(1);
    require(node.kind == json::Kind::number && parsed.lexeme(node) == token, "Number lost its lexical span");
    require(node.real == expected && std::signbit(node.real) == std::signbit(expected), "Binary64 value differs");
}

void integer(std::string_view token, IntegerStatus status, std::int32_t expected = 0) {
    const auto parsed = accept(field(token));
    const auto result = json::integer32(parsed, parsed.nodes.at(1));
    require(result.status == status && result.value == expected, "Exact signed 32-bit classification differs");
}

void lexical() {
    const std::string duplicate = "{\r\n  \"\xc3\xa9\":0,\"na\\u006de\":1,\"name\":2}";
    const auto error = reject(duplicate, "CFG_DUPLICATE_KEY", duplicate.find("\"name\""), "/name");
    require(error.first_key_offset == duplicate.find("\"na\\u006de\""), "First decoded key offset differs");
    require(error.diagnostic.line == 2 && error.diagnostic.column == 23 &&
            error.diagnostic.field == "name", "Duplicate source location differs");
    const std::string nested = R"JSON({"~/":[{"\u006b":1,"k":2}]})JSON";
    const auto inside = reject(nested, "CFG_DUPLICATE_KEY", nested.find("\"k\""), "/~0~1/0/k");
    require(inside.first_key_offset == nested.find("\"\\u006b\""), "Nested first key offset differs");
    const std::string unicode_keys = "{\"\xf0\x9f\x98\x80\":0,\"\\uD83D\\uDE00\":1}";
    reject(unicode_keys, "CFG_DUPLICATE_KEY", unicode_keys.find("\"\\u"), "/\xf0\x9f\x98\x80");

    std::string source = R"JSON({"a":{"x":0},"b":[{"x":1},true,false,null],"s":"\uD83D\uDE00","e":"\"\\\/\b\f\n\r\t"})JSON";
    const auto valid = accept(source);
    source.assign(source.size(), 'x');
    require(valid.nodes.at(0).after == valid.nodes.size() && valid.nodes.at(1).after == 3 &&
            valid.nodes.at(4).after == 6 && valid.nodes.at(6).boolean && !valid.nodes.at(7).boolean &&
            valid.nodes.at(8).kind == json::Kind::null, "Preorder child spans differ");
    require(valid.names.at(valid.nodes.at(2).key).value == "x" &&
            valid.names.at(valid.nodes.at(5).key).value == "x", "Separate object scopes lost repeated key");
    require(valid.strings.at(valid.nodes.at(9).text) == "\xf0\x9f\x98\x80" &&
            valid.lexeme(valid.nodes.at(9)) == R"JSON("\uD83D\uDE00")JSON",
            "Escaped Unicode value or owned source differs");
    require(valid.strings.at(valid.nodes.at(10).text) == "\"\\/\b\f\n\r\t", "Short escape decoding differs");

    reject("\xef\xbb\xbf{}", "CFG_UTF8", 0);
    reject(field(R"JSON("\uD800")JSON"), "CFG_UTF8", 6, "/n");
    reject(field(R"JSON("\uDC00")JSON"), "CFG_UTF8", 6, "/n");
    reject(field(R"JSON("\uD800\u0041")JSON"), "CFG_UTF8", 12, "/n");
    reject(field(R"JSON("\u0000")JSON"), "CFG_UTF8", 6, "/n");
    reject(field(std::string("\"\0\"", 3)), "CFG_UTF8", 6, "/n");
    reject(field("\"\xc0\xaf\""), "CFG_UTF8", 6, "/n");
    reject(field("\"\xe0\x80\x80\""), "CFG_UTF8", 6, "/n");
    reject(field("\"\xed\xa0\x80\""), "CFG_UTF8", 6, "/n");
    reject(field("\"\xf4\x90\x80\x80\""), "CFG_UTF8", 6, "/n");
    reject(field("\"\xe2\x82\""), "CFG_UTF8", 8, "/n");
    reject(field("\xff"), "CFG_UTF8", 5, "/n");
    reject(field(R"JSON("\x41")JSON"), "CFG_SYNTAX", 7, "/n");
    reject(field(R"JSON("\u01x2")JSON"), "CFG_SYNTAX", 10, "/n");
    reject(field("\"\n\""), "CFG_SYNTAX", 6, "/n");
    reject("[]", "CFG_SYNTAX", 0);
    reject("42", "CFG_SYNTAX", 0);
    reject("null", "CFG_SYNTAX", 0);
    reject("", "CFG_SYNTAX", 0);
    reject("{} {}", "CFG_SYNTAX", 3);
    reject("{/* comment */}", "CFG_SYNTAX", 1);
    reject("{} // comment", "CFG_SYNTAX", 3);
    reject("{\"n\":1,}", "CFG_SYNTAX", 7);
    reject(field("[0,]"), "CFG_SYNTAX", 8, "/n/1");
    reject(field("NaN"), "CFG_SYNTAX", 5, "/n");
    reject(field("Infinity"), "CFG_SYNTAX", 5, "/n");
    reject(field("+1"), "CFG_SYNTAX", 5, "/n");
    reject(field("01"), "CFG_SYNTAX", 6, "/n");
    reject(field("0."), "CFG_SYNTAX", 7, "/n");
    reject(field("1e+"), "CFG_SYNTAX", 8, "/n");
    reject(field("1.7976931348623159e308"), "CFG_NUMBER", 5, "/n");
    reject(field("1e99999"), "CFG_NUMBER", 5, "/n");
    reject(field("2e-324"), "CFG_NUMBER", 5, "/n");
    reject(field("-1e-99999"), "CFG_NUMBER", 5, "/n");

    using Limits = std::numeric_limits<double>;
    real("1.7976931348623157e308", Limits::max());
    real("-1.7976931348623157e308", Limits::lowest());
    real("2.2250738585072014e-308", Limits::min());
    real("4.9406564584124654e-324", Limits::denorm_min());
    real("-5e-324", -Limits::denorm_min());
    real("0", 0.0);
    real("-0", -0.0);
    real("-0.0", -0.0);
    real("-0e99999", -0.0);
    real("0e-99999", 0.0);
    real("1.00000000000000011102230246251565404236316680908203125", 1.0);
    real("1.00000000000000011102230246251565404236316680908203126", std::nextafter(1.0, 2.0));

    integer("1.00000000000000000001", IntegerStatus::fractional);
    integer("1e-100", IntegerStatus::fractional);
    integer("1.0", IntegerStatus::exact, 1);
    integer("10e-1", IntegerStatus::exact, 1);
    integer("0.001e3", IntegerStatus::exact, 1);
    integer("-0e-99999", IntegerStatus::exact);
    integer("2147483647", IntegerStatus::exact, INT32_MAX);
    integer("-2147483648.0", IntegerStatus::exact, INT32_MIN);
    integer("2147483648", IntegerStatus::out_of_range);
    integer("-2147483649", IntegerStatus::out_of_range);
    integer("123456789012345678901234567890123456789012345678901234567890", IntegerStatus::out_of_range);
}

std::string scalar_array(std::size_t count) {
    std::string text = "[" + std::string(count * 2, ',');
    for (std::size_t i = 1; i < text.size(); i += 2) text[i] = '0';
    text.back() = ']';
    return text;
}

std::string object(std::size_t count) {
    std::string text = "{";
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) text += ',';
        text += '"' + std::to_string(i) + "\":0";
    }
    return text + '}';
}

void limits() {
    for (auto size : {json::document_bytes - 1, json::document_bytes}) {
        std::string input = "{}" + std::string(size - 2, ' ');
        require(accept(input).nodes.size() == 1, "Wrong document boundary");
    }
    reject("{}" + std::string(json::document_bytes - 1, ' '), "CFG_LIMIT", json::document_bytes);

    for (auto depth : {json::container_depth - 2, json::container_depth - 1}) {
        const auto input = field(std::string(depth, '[') + '0' + std::string(depth, ']'));
        require(accept(input).nodes.size() == 2 + depth, "Wrong nested value count");
    }
    std::string path = "/n";
    for (std::size_t i = 1; i < json::container_depth; ++i) path += "/0";
    reject(field(std::string(json::container_depth, '[') + '0' +
                 std::string(json::container_depth, ']')), "CFG_LIMIT", 4 + json::container_depth, path);

    const std::string scalar = "\xf0\x9f\x98\x80";
    std::string decoded;
    for (std::size_t i = 0; i < json::string_scalars - 1; ++i) decoded += scalar;
    require(accept(field('"' + decoded + '"')).strings.at(0) == decoded, "String below limit differs");
    require(accept(field('"' + decoded + "\\uD83D\\uDE00\"")).strings.at(0) == decoded + scalar,
            "Surrogate pair did not count as one scalar");
    const auto at_limit = decoded + scalar;
    reject(field('"' + at_limit + scalar + '"'), "CFG_LIMIT", 6 + at_limit.size(), "/n");

    for (auto size : {json::number_bytes - 1, json::number_bytes})
        real("1." + std::string(size - 2, '0'), 1.0);
    reject(field("1." + std::string(json::number_bytes - 1, '0')),
           "CFG_LIMIT", 5 + json::number_bytes, "/n");

    for (auto size : {json::object_members - 1, json::object_members})
        require(accept(object(size)).names.size() == size, "Wrong member count");
    const auto large_object = object(json::object_members + 1);
    reject(large_object, "CFG_LIMIT", large_object.find("\"256\""));

    for (auto size : {json::array_items - 1, json::array_items})
        require(accept(field(scalar_array(size))).nodes.size() == 2 + size, "Wrong array count");
    reject(field(scalar_array(json::array_items + 1)), "CFG_LIMIT", 6 + 2 * json::array_items,
           "/n/" + std::to_string(json::array_items));

    const std::string prefix = "{\"a\":" + scalar_array(json::array_items) + ",\"b\":";
    const auto second_size = json::parsed_values - json::array_items - 3;
    for (auto size : {second_size - 1, second_size})
        require(accept(prefix + scalar_array(size) + '}').nodes.size() == 3 + json::array_items + size,
                "Wrong aggregate value count");
    reject(prefix + scalar_array(second_size + 1) + '}', "CFG_LIMIT",
           prefix.size() + 1 + 2 * second_size, "/b/" + std::to_string(second_size));
}
}

int main(int argc, char **argv) {
    try {
        const auto mode = argc == 2 ? std::string_view(argv[1]) : "";
        if (mode == "lexical") lexical();
        else if (mode == "limits") limits();
        else return 2;
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
