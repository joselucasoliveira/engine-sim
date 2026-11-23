#ifndef ENGINE_SIM_JSON_SCHEMA_HPP
#define ENGINE_SIM_JSON_SCHEMA_HPP

#include "lexical.hpp"

namespace engine_sim::config_v1::json_detail {

struct Value {
    const ParsedJson &json;
    std::uint32_t index;

    const Node &node() const noexcept { return json.nodes[index]; }
    Kind kind() const noexcept { return node().kind; }
    std::string_view text() const noexcept { return json.strings[node().text]; }
    std::string_view lexeme() const noexcept { return json.lexeme(node()); }
    std::optional<Value> member(std::string_view name) const;
};

enum class Type { any, array, boolean, integer, number, object, string, null };

struct Constant {
    Kind kind;
    std::string_view spelling;
};

struct Property {
    std::string_view name;
    int rule;
    bool required;
};

struct Rule {
    Type type = Type::any;
    std::optional<Constant> constant;
    int reference = -1;
    int items = -1;
    std::vector<int> alternatives;
    std::vector<Property> properties;
    bool closed = false;
    bool discriminated = false;
    std::optional<std::string_view> minimum, maximum, exclusiveMinimum;
    std::optional<std::size_t> minItems, maxItems, minLength, maxLength;
};
using Rules = std::vector<Rule>;

// Shared keyword evaluator. Arbitrary rule inputs are built only by offline development tooling.
std::optional<Diagnostic> validate_shape(const Rules &rules, Value value);
std::optional<Diagnostic> validate_v1(const ParsedJson &parsed);
void locate_diagnostic(const ParsedJson &parsed, Diagnostic &problem);

}
#endif
