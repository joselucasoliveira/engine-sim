#include "schema.hpp"
#include "read.hpp"
#include "config_schema_v1.hpp"
#include "config_schema_v2.hpp"

#include <algorithm>
#include <charconv>

namespace engine_sim::config_v1::json_detail {

std::optional<Value> Value::member(std::string_view name) const {
    if (kind() != Kind::object) return {};
    for (auto i = index + 1; i < node().after; i = json.nodes[i].after)
        if (json.names[json.nodes[i].key].value == name) return Value{json, i};
    return {};
}

namespace {
struct PathPart {
    std::string_view key;
    std::size_t index;
    bool is_key;
};

bool matches_type(Type type, Value value) {
    switch (type) {
        case Type::any: return true;
        case Type::array: return value.kind() == Kind::array;
        case Type::boolean: return value.kind() == Kind::boolean;
        case Type::integer:
            return value.kind() == Kind::number &&
                   integer32(value.json, value.node()).status != IntegerStatus::fractional;
        case Type::number: return value.kind() == Kind::number;
        case Type::object: return value.kind() == Kind::object;
        case Type::string: return value.kind() == Kind::string;
        case Type::null: return value.kind() == Kind::null;
    }
    return false;
}

bool matches_constant(const Constant &constant, Value value) {
    if (value.kind() != constant.kind) return false;
    if (constant.kind == Kind::number) return compare_numbers(value.lexeme(), constant.spelling) == 0;
    if (constant.kind == Kind::string) return value.text() == constant.spelling;
    if (constant.kind == Kind::boolean) return value.node().boolean == (constant.spelling == "true");
    return constant.kind == Kind::null;
}

class Validator {
public:
    explicit Validator(const Rules &rules) : m_rules(rules) {}
    std::optional<Diagnostic> result(Value value) { return check(0, value); }

private:
    Diagnostic fail(Value value, std::string_view message, std::optional<std::size_t> offset) const {
        Diagnostic problem;
        problem.stage = "shape";
        problem.code = "CFG_SHAPE";
        problem.message = message;
        for (const auto &part : m_path) {
            problem.pointer += '/';
            if (part.is_key) append_pointer_key(problem.pointer, part.key);
            else problem.pointer += std::to_string(part.index);
        }
        if (!m_path.empty() && m_path.back().is_key) problem.field = m_path.back().key;
        if (offset) set_source_location(problem, value.json.bytes, *offset);
        return problem;
    }

    Diagnostic fail(Value value, std::string_view message) const {
        return fail(value, message, value.node().span.begin);
    }

    std::optional<Diagnostic> variants(const Rule &rule, Value value) {
        if (rule.discriminated) {
            const auto discriminator = value.member("kind");
            if (discriminator) {
                for (const auto index : rule.alternatives) {
                    const auto &arm = m_rules[static_cast<std::size_t>(index)];
                    const auto &target = m_rules[static_cast<std::size_t>(arm.reference)];
                    const auto kind = std::find_if(target.properties.begin(), target.properties.end(),
                                                  [](const auto &property) { return property.name == "kind"; });
                    const auto &constant = m_rules[static_cast<std::size_t>(kind->rule)].constant.value();
                    if (matches_constant(constant, *discriminator)) return check(index, value);
                }
            }
            if (value.kind() != Kind::object) return fail(value, "Expected a discriminated object");
            m_path.push_back({"kind", 0, true});
            auto problem = discriminator ? fail(*discriminator, "Unrecognized variant discriminator")
                                         : fail(value, "Missing variant discriminator", {});
            m_path.pop_back();
            return problem;
        }

        std::size_t matching = 0;
        for (const auto index : rule.alternatives)
            if (!check(index, value) && ++matching == 2) break;
        if (matching != 1) return fail(value, "Expected exactly one matching schema alternative");
        return {};
    }

    std::optional<Diagnostic> object(const Rule &rule, Value value) {
        for (const auto &property : rule.properties) {
            const auto member = value.member(property.name);
            if (!member && !property.required) continue;
            if (member && property.rule < 0) continue;
            m_path.push_back({property.name, 0, true});
            auto problem = member ? check(property.rule, *member) :
                                    std::optional{fail(value, "Required property is absent", {})};
            m_path.pop_back();
            if (problem) return problem;
        }
        if (!rule.closed) return {};
        for (auto i = value.index + 1; i < value.node().after; i = value.json.nodes[i].after) {
            const auto &name = value.json.names[value.json.nodes[i].key];
            const auto known = std::any_of(rule.properties.begin(), rule.properties.end(),
                                           [&](const auto &property) { return property.rule >= 0 && name.value == property.name; });
            if (!known) {
                m_path.push_back({name.value, 0, true});
                auto problem = fail(value, "Property is outside the closed schema", name.span.begin);
                m_path.pop_back();
                return problem;
            }
        }
        return {};
    }

    std::optional<Diagnostic> array(const Rule &rule, Value value) {
        std::size_t count = 0;
        for (auto i = value.index + 1; i < value.node().after; i = value.json.nodes[i].after) ++count;
        if ((rule.minItems && count < *rule.minItems) || (rule.maxItems && count > *rule.maxItems))
            return fail(value, "Array length is outside the schema bounds");
        if (rule.items < 0) return {};
        std::size_t position = 0;
        for (auto i = value.index + 1; i < value.node().after; i = value.json.nodes[i].after, ++position) {
            m_path.push_back({{}, position, false});
            auto problem = check(rule.items, {value.json, i});
            m_path.pop_back();
            if (problem) return problem;
        }
        return {};
    }

    std::optional<Diagnostic> check(int index, Value value) {
        const auto &rule = m_rules[static_cast<std::size_t>(index)];
        if (rule.reference >= 0)
            if (auto problem = check(rule.reference, value)) return problem;
        if (!matches_type(rule.type, value)) return fail(value, "Value has the wrong schema type");
        if (rule.constant && !matches_constant(*rule.constant, value))
            return fail(value, "Value differs from the schema constant");
        if (!rule.alternatives.empty())
            if (auto problem = variants(rule, value)) return problem;

        switch (value.kind()) {
            case Kind::object: return object(rule, value);
            case Kind::array: return array(rule, value);
            case Kind::string: {
                const auto text = value.text();
                if (!rule.choices.empty() && std::find(rule.choices.begin(), rule.choices.end(), text) == rule.choices.end())
                    return fail(value, "Value is outside the schema choices");
                if (!rule.minLength && !rule.maxLength) break;
                const auto length = static_cast<std::size_t>(std::count_if(text.begin(), text.end(),
                    [](unsigned char c) { return (c & 0xc0) != 0x80; }));
                if ((rule.minLength && length < *rule.minLength) || (rule.maxLength && length > *rule.maxLength))
                    return fail(value, "String length is outside the schema bounds");
                break;
            }
            case Kind::number: {
                const auto token = value.lexeme();
                if ((rule.minimum && compare_numbers(token, *rule.minimum) < 0) ||
                    (rule.maximum && compare_numbers(token, *rule.maximum) > 0) ||
                    (rule.exclusiveMinimum && compare_numbers(token, *rule.exclusiveMinimum) <= 0))
                    return fail(value, "Number is outside the schema bounds");
                break;
            }
            default: break;
        }
        return {};
    }

    const Rules &m_rules;
    std::vector<PathPart> m_path;
};

std::string unescape_pointer_token(std::string_view token) {
    std::string name;
    for (std::size_t i = 0; i < token.size(); ++i) {
        if (token[i] == '~' && i + 1 < token.size()) name += token[++i] == '0' ? '~' : '/';
        else name += token[i];
    }
    return name;
}
}

std::optional<Diagnostic> validate_shape(const Rules &rules, Value value) {
    return Validator(rules).result(value);
}

std::optional<Diagnostic> validate_document(const ParsedJson &parsed, int &format, bool allow_authoring) {
    const Value root{parsed, 0};
    const auto version = root.member("format_version");
    if (!version || version->kind() != Kind::number ||
        integer32(parsed, version->node()).status == IntegerStatus::fractional) {
        Diagnostic problem{"CFG_SHAPE", "shape", "/format_version", "Expected an integer format version",
                           {}, "format_version", {}, {}, {}, {}};
        if (version) set_source_location(problem, parsed.bytes, version->node().span.begin);
        return problem;
    }
    if (compare_numbers(version->lexeme(), "1") == 0) {
        format = 1;
    } else if (allow_authoring && compare_numbers(version->lexeme(), "2") == 0) {
        format = 2;
    } else {
        Diagnostic problem{"CFG_VERSION", "version", "/format_version", "Unsupported engine configuration version",
                           {}, "format_version", {}, {}, {}, {}};
        set_source_location(problem, parsed.bytes, version->node().span.begin);
        return problem;
    }
    if (format == 2) {
        static const auto rules = make_v2_rules();
        return validate_shape(rules, root);
    }
    static const auto rules = make_v1_rules();
    return validate_shape(rules, root);
}

std::optional<Diagnostic> validate_v1(const ParsedJson &parsed) {
    int format = 0;
    return validate_document(parsed, format, false);
}

std::string_view schema_digest() noexcept { return embedded_schema_digest; }
std::string_view schema_bytes() noexcept { return embedded_schema_bytes; }

void locate_diagnostic(const ParsedJson &parsed, Diagnostic &problem) {
    auto trail = std::string_view(problem.pointer);
    if (!trail.empty() && trail.front() != '/') return;
    std::uint32_t index = 0;
    while (!trail.empty()) {
        trail.remove_prefix(1);
        const auto next = trail.find('/');
        const auto token = trail.substr(0, next);
        const Value value{parsed, index};
        if (value.kind() == Kind::object) {
            const auto member = value.member(unescape_pointer_token(token));
            if (!member) return;
            index = member->index;
        }
        else if (value.kind() == Kind::array) {
            std::size_t wanted = 0;
            const auto result = std::from_chars(token.data(), token.data() + token.size(), wanted);
            if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) return;
            ++index;
            while (wanted > 0 && index < value.node().after) {
                index = parsed.nodes[index].after;
                --wanted;
            }
            if (index == value.node().after) return;
        }
        else return;
        trail = next == std::string_view::npos ? std::string_view{} : trail.substr(next);
    }
    set_source_location(problem, parsed.bytes, parsed.nodes[index].span.begin);
}

}
