#include "read.hpp"
#include "schema.hpp"
#include "record_fields.hpp"

#include <type_traits>
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
struct Decode {
    static T from(Value value) {
        T output;
        RecordFields<T>{}(output, [&](std::string_view key, auto &field) {
            using Field = std::remove_reference_t<decltype(field)>;
            if constexpr (!std::is_const_v<Field>) assign(value, key, field);
        });
        return output;
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
