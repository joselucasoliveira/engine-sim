#ifndef ENGINE_SIM_JSON_LEXICAL_HPP
#define ENGINE_SIM_JSON_LEXICAL_HPP

#include <engine_sim/config-v1.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace engine_sim::config_v1::json_detail {

inline constexpr std::size_t document_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t container_depth = 64;
inline constexpr std::size_t string_scalars = 4'096;
inline constexpr std::size_t parsed_values = 2'097'152;
inline constexpr std::size_t number_bytes = 128;
inline constexpr std::size_t object_members = 256;
inline constexpr std::size_t array_items = 1'048'576;

enum class Kind { object, array, string, number, boolean, null };

struct Span {
    std::uint32_t begin{};
    std::uint32_t end{};
};

inline constexpr auto no_key = std::numeric_limits<std::uint32_t>::max();

struct Node {
    Kind kind{};
    Span span{};
    // In the preorder tape, children run from the next index up to after.
    std::uint32_t after{};
    std::uint32_t key = no_key;
    std::uint32_t text{};
    double real{};
    bool boolean{};
};

struct Name {
    std::string value;
    Span span;
};

struct ParsedJson {
    std::string bytes;
    std::vector<Node> nodes;
    std::vector<Name> names;
    std::vector<std::string> strings;

    std::string_view lexeme(const Node &node) const noexcept {
        return std::string_view(bytes).substr(node.span.begin, node.span.end - node.span.begin);
    }
};

struct ParseError {
    Diagnostic diagnostic;
    std::optional<std::uint32_t> first_key_offset;
};

using ParseResult = std::variant<ParsedJson, ParseError>;
ParseResult parse(std::string_view bytes);
void append_pointer_key(std::string &pointer, std::string_view key);
void set_source_location(Diagnostic &problem, std::string_view input, std::size_t offset);

enum class IntegerStatus { exact, fractional, out_of_range };

struct Integer32 {
    IntegerStatus status{};
    std::int32_t value{};
};

Integer32 integer32(const ParsedJson &input, const Node &node) noexcept;
// Compare two admitted decimal spellings without passing through binary64.
int compare_numbers(std::string_view a, std::string_view b) noexcept;

}
#endif
