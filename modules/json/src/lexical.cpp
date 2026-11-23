#include "lexical.hpp"

#include <json.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace engine_sim::config_v1::json_detail {
namespace {
static_assert(NLOHMANN_JSON_VERSION_MAJOR == 3 &&
              NLOHMANN_JSON_VERSION_MINOR == 12 && NLOHMANN_JSON_VERSION_PATCH == 0);

bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool delimiter(char c) { return space(c) || c == ',' || c == ']' || c == '}'; }

std::uint32_t narrow(std::size_t value) { return static_cast<std::uint32_t>(value); }

struct PathPart {
    std::string_view name{};
    std::size_t index{};
    bool is_name{};
};

void append_pointer_key(std::string &pointer, std::string_view key) {
    for (char c : key) {
        if (c == '~') pointer += "~0";
        else if (c == '/') pointer += "~1";
        else pointer += c;
    }
}

class Reader {
public:
    explicit Reader(std::string_view bytes) : m_input(bytes) {}

    ParsedJson run() {
        if (m_input.size() > document_bytes) fail("CFG_LIMIT", document_bytes, "Document exceeds the byte limit");
        if (m_input.substr(0, 3) == "\xef\xbb\xbf") fail("CFG_UTF8", 0, "A JSON document cannot begin with a BOM");
        skip();
        if (peek() != '{') unexpected("Engine configuration must begin with an object");
        value(0, no_key);
        skip();
        if (!done()) unexpected("Unexpected data after the document");
        m_result.bytes = m_input;
        return std::move(m_result);
    }

private:
    [[noreturn]] void fail(std::string_view code, std::size_t offset, std::string_view message,
                           std::optional<std::uint32_t> first_key = {}) const {
        Diagnostic problem;
        problem.code = code;
        problem.stage = "parse";
        problem.message = message;
        for (const auto &part : m_path) {
            problem.pointer += '/';
            if (part.is_name) append_pointer_key(problem.pointer, part.name);
            else problem.pointer += std::to_string(part.index);
        }
        if (!m_path.empty() && m_path.back().is_name) problem.field = m_path.back().name;
        problem.byte_offset = offset;
        std::uint64_t line = 1, column = 1;
        for (std::size_t i = 0; i < offset; ++i) {
            const auto c = static_cast<unsigned char>(m_input[i]);
            if (c == '\r' || c == '\n') {
                if (c == '\r' && i + 1 < offset && m_input[i + 1] == '\n') ++i;
                ++line;
                column = 1;
            }
            else if ((c & 0xc0) != 0x80) ++column;
        }
        problem.line = line;
        problem.column = column;
        throw ParseError{std::move(problem), first_key};
    }

    bool done() const { return m_pos == m_input.size(); }
    char peek() const { return done() ? '\0' : m_input[m_pos]; }
    bool consume(char c) {
        if (done() || peek() != c) return false;
        ++m_pos;
        return true;
    }
    void skip() {
        while (!done() && space(peek())) ++m_pos;
    }
    void need(char c, std::string_view text) {
        if (!consume(c)) unexpected(text);
    }

    [[noreturn]] void unexpected(std::string_view message) { unexpected(message, m_pos); }

    [[noreturn]] void unexpected(std::string_view message, std::size_t offset) {
        if (offset < m_input.size()) {
            const auto c = static_cast<unsigned char>(m_input[offset]);
            if (c == 0) fail("CFG_UTF8", offset, "NUL is forbidden in a JSON byte span");
            if (c >= 0x80) {
                m_pos = offset + 1;
                utf8_tail(c, offset);
            }
        }
        fail("CFG_SYNTAX", offset, message);
    }

    void literal(std::string_view text) {
        for (char c : text) need(c, "Invalid JSON literal");
        if (!done() && !delimiter(peek()))
            unexpected("Invalid character after a JSON literal");
    }

    void value(std::size_t depth, std::uint32_t key) {
        const auto begin = m_pos;
        if (m_result.nodes.size() == parsed_values)
            fail("CFG_LIMIT", begin, "Document exceeds the parsed value limit");
        if ((peek() == '{' || peek() == '[') && depth == container_depth)
            fail("CFG_LIMIT", begin, "Document exceeds the container depth limit");
        const auto index = m_result.nodes.size();
        m_result.nodes.emplace_back();
        Node node;
        node.span.begin = narrow(begin);
        node.key = key;
        switch (peek()) {
            case '{':
                node.kind = Kind::object;
                object(depth + 1);
                break;
            case '[':
                node.kind = Kind::array;
                array(depth + 1);
                break;
            case '"': {
                node.kind = Kind::string;
                node.text = narrow(m_result.strings.size());
                auto token = string();
                m_result.strings.push_back(std::move(token.value));
                break;
            }
            case 't':
            case 'f':
                node.kind = Kind::boolean;
                node.boolean = peek() == 't';
                literal(node.boolean ? "true" : "false");
                break;
            case 'n':
                node.kind = Kind::null;
                literal("null");
                break;
            default:
                if (peek() != '-' && !digit(peek())) unexpected("Expected a JSON value");
                node.kind = Kind::number;
                node.real = number();
                break;
        }
        node.span.end = narrow(m_pos);
        node.after = narrow(m_result.nodes.size());
        m_result.nodes[index] = node;
    }

    void object(std::size_t depth) {
        ++m_pos; // opening brace
        skip();
        if (consume('}')) return;
        std::unordered_map<std::string, std::uint32_t> seen;
        while (true) {
            if (peek() != '"') unexpected("Expected a quoted object key");
            if (seen.size() == object_members)
                fail("CFG_LIMIT", m_pos, "Object exceeds the member limit");
            auto member = string();
            const auto [entry, fresh] = seen.emplace(member.value, member.span.begin);
            m_path.push_back({entry->first, 0, true});
            if (!fresh)
                fail("CFG_DUPLICATE_KEY", member.span.begin,
                     "Object key was already defined at byte " + std::to_string(entry->second), entry->second);
            skip();
            need(':', "Expected a colon after the object key");
            skip();
            const auto index = narrow(m_result.names.size());
            m_result.names.push_back(std::move(member));
            value(depth, index);
            m_path.pop_back();
            skip();
            if (consume('}')) return;
            need(',', "Expected a comma between object members");
            skip();
        }
    }

    void array(std::size_t depth) {
        ++m_pos; // opening bracket
        skip();
        if (consume(']')) return;
        for (std::size_t i = 0;; ++i) {
            m_path.push_back({{}, i, false});
            if (i == array_items) fail("CFG_LIMIT", m_pos, "Array exceeds the item limit");
            value(depth, no_key);
            m_path.pop_back();
            skip();
            if (consume(']')) return;
            need(',', "Expected a comma between array values");
            skip();
        }
    }

    std::uint32_t hex4() {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            if (done()) unexpected("Incomplete Unicode escape");
            const char c = peek();
            unsigned nibble = 0;
            if ('0' <= c && c <= '9') nibble = c - '0';
            else if ('a' <= c && c <= 'f') nibble = c - 'a' + 10;
            else if ('A' <= c && c <= 'F') nibble = c - 'A' + 10;
            else unexpected("Expected a hexadecimal digit in the Unicode escape");
            ++m_pos;
            value = (value << 4) | nibble;
        }
        return value;
    }

    void escape(std::size_t begin) {
        if (done()) unexpected("Incomplete escape");
        const char code = m_input[m_pos++];
        if (code != 'u') {
            if (std::string_view("\"\\/bfnrt").find(code) == std::string_view::npos)
                unexpected("Invalid JSON escape", m_pos - 1);
            return;
        }
        const auto first = hex4();
        if (first == 0) fail("CFG_UTF8", begin, "Escaped NUL is forbidden");
        if (first >= 0xdc00 && first <= 0xdfff)
            fail("CFG_UTF8", begin, "Unpaired low surrogate");
        if (first >= 0xd800 && first <= 0xdbff) {
            const auto second_offset = m_pos;
            if (!consume('\\') || !consume('u'))
                fail("CFG_UTF8", begin, "High surrogate requires a low surrogate escape");
            const auto second = hex4();
            if (second < 0xdc00 || second > 0xdfff)
                fail("CFG_UTF8", second_offset, "Expected a low surrogate");
        }
    }

    void utf8_tail(unsigned char first, std::size_t begin) {
        unsigned rest = 0;
        std::uint32_t point = 0;
        if (first >= 0xc2 && first <= 0xdf) { rest = 1; point = first & 0x1f; }
        else if (first >= 0xe0 && first <= 0xef) { rest = 2; point = first & 0x0f; }
        else if (first >= 0xf0 && first <= 0xf4) { rest = 3; point = first & 0x07; }
        else fail("CFG_UTF8", begin, "Invalid leading UTF-8 byte");
        if (rest > m_input.size() - m_pos) fail("CFG_UTF8", begin, "Truncated UTF-8 sequence");
        const auto length = rest;
        while (rest > 0) {
            const auto next = static_cast<unsigned char>(peek());
            if ((next & 0xc0) != 0x80) fail("CFG_UTF8", m_pos, "Invalid UTF-8 continuation byte");
            ++m_pos;
            point = (point << 6) | (next & 0x3f);
            --rest;
        }
        if ((length == 2 && point < 0x800) || (length == 3 && point < 0x10000) ||
            (point >= 0xd800 && point <= 0xdfff) || point > 0x10ffff)
            fail("CFG_UTF8", begin, "Invalid Unicode scalar in UTF-8 sequence");
    }

    Name string() {
        const auto begin = m_pos++;
        std::size_t count = 0;
        while (!done()) {
            const auto offset = m_pos;
            const auto c = static_cast<unsigned char>(m_input[m_pos++]);
            if (c == '"') {
                const auto token = m_input.substr(begin, m_pos - begin);
                // The pinned decoder sees only a bounded string token. Object
                // identities and original byte spans stay in this reader.
                auto decoded = nlohmann::json::parse(token.begin(), token.end(), nullptr, false);
                if (!decoded.is_string()) fail("CFG_UTF8", begin, "String decoding rejected the token");
                return {std::move(decoded.get_ref<std::string &>()), {narrow(begin), narrow(m_pos)}};
            }
            if (++count > string_scalars) fail("CFG_LIMIT", offset, "String exceeds the Unicode scalar limit");
            if (c == '\\') escape(offset);
            else if (c == 0) fail("CFG_UTF8", offset, "NUL is forbidden in a JSON string");
            else if (c < 0x20) fail("CFG_SYNTAX", offset, "Unescaped control byte in string");
            else if (c >= 0x80) utf8_tail(c, offset);
        }
        unexpected("Unterminated JSON string");
    }

    void number_char(std::size_t begin) {
        if (m_pos - begin == number_bytes)
            fail("CFG_LIMIT", m_pos, "Number exceeds the token length limit");
        ++m_pos;
    }

    void number_digits(std::size_t begin, bool &nonzero) {
        if (!digit(peek())) unexpected("Expected a decimal digit");
        while (digit(peek())) {
            nonzero = nonzero || peek() != '0';
            number_char(begin);
        }
    }

    double number() {
        const auto begin = m_pos;
        const bool negative = peek() == '-';
        if (negative) number_char(begin);
        bool nonzero = false;
        if (peek() == '0') {
            number_char(begin);
            if (digit(peek())) unexpected("A JSON integer cannot have a leading zero");
        }
        else number_digits(begin, nonzero);
        if (peek() == '.') {
            number_char(begin);
            number_digits(begin, nonzero);
        }
        if (peek() == 'e' || peek() == 'E') {
            number_char(begin);
            if (peek() == '-' || peek() == '+') number_char(begin);
            bool discarded = false;
            number_digits(begin, discarded);
        }
        if (!done() && !delimiter(peek()))
            unexpected("Invalid character after number");
        if (!nonzero) return negative ? -0.0 : 0.0;
        const auto token = m_input.substr(begin, m_pos - begin);
        double real = 0;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), real, std::chars_format::general);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size() ||
            !std::isfinite(real) || real == 0)
            fail("CFG_NUMBER", begin, "Number is outside the nonzero finite binary64 range");
        return real;
    }

    const std::string_view m_input;
    std::size_t m_pos = 0;
    std::vector<PathPart> m_path;
    ParsedJson m_result;
};
}

ParseResult parse(std::string_view bytes) {
    try { return Reader(bytes).run(); }
    catch (ParseError &error) { return std::move(error); }
}

Integer32 integer32(const ParsedJson &input, const Node &node) noexcept {
    if (node.kind != Kind::number) return {IntegerStatus::fractional, 0};
    const auto token = input.lexeme(node);
    const bool negative = token.front() == '-';
    std::array<unsigned char, number_bytes> digits{};
    std::size_t i = negative ? 1 : 0, count = 0;
    int fractional = 0;
    bool after_point = false;
    for (; i < token.size() && token[i] != 'e' && token[i] != 'E'; ++i) {
        if (token[i] == '.') after_point = true;
        else {
            digits[count++] = static_cast<unsigned char>(token[i] - '0');
            if (after_point) ++fractional;
        }
    }
    int exponent = 0;
    if (i < token.size()) {
        ++i;
        const bool exp_negative = token[i] == '-';
        if (exp_negative || token[i] == '+') ++i;
        for (; i < token.size() && exponent < 1000; ++i)
            exponent = exponent * 10 + token[i] - '0';
        if (exp_negative) exponent = -exponent;
    }
    std::size_t first = 0;
    while (first < count && digits[first] == 0) ++first;
    if (first == count) return {IntegerStatus::exact, 0};

    const int scale = exponent - fractional;
    if (scale < 0) {
        const auto remove = static_cast<std::size_t>(-scale);
        if (remove >= count - first) return {IntegerStatus::fractional, 0};
        for (i = count - remove; i < count; ++i)
            if (digits[i] != 0) return {IntegerStatus::fractional, 0};
        count -= remove;
    }
    else if (count - first + static_cast<std::size_t>(scale) > 10)
        return {IntegerStatus::out_of_range, 0};

    if (count - first > 10) return {IntegerStatus::out_of_range, 0};
    std::uint64_t magnitude = 0;
    for (i = first; i < count; ++i) magnitude = magnitude * 10 + digits[i];
    for (int power = 0; power < scale; ++power) magnitude *= 10;
    const auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + negative;
    if (magnitude > limit) return {IntegerStatus::out_of_range, 0};
    const auto value = static_cast<std::int64_t>(magnitude);
    return {IntegerStatus::exact, static_cast<std::int32_t>(negative ? -value : value)};
}

}
