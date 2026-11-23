#ifndef ENGINE_SIM_CONFIG_VALIDATION_SUPPORT_HPP
#define ENGINE_SIM_CONFIG_VALIDATION_SUPPORT_HPP

#include <engine_sim/config-validation.hpp>

#include <string_view>

namespace engine_sim::config_v1::detail {

inline constexpr std::size_t definition_limit = 65'536;
inline constexpr std::size_t cylinder_limit = 1'024;
inline constexpr std::size_t curve_sample_limit = 1'048'576;

struct Cursor {
    std::string pointer;
    std::string_view member{};
    std::string_view component{};

    Cursor field(std::string_view key) const {
        auto next = *this;
        next.pointer += '/';
        next.pointer += key;
        next.member = key;
        return next;
    }

    Cursor item(std::size_t index) const {
        auto next = *this;
        next.pointer += '/';
        next.pointer += std::to_string(index);
        return next;
    }

    Cursor owner(std::string_view id) const {
        auto next = *this;
        next.component = id;
        return next;
    }
};

struct InvalidModel {
    Diagnostic detail;
};

[[noreturn]] void reject(const Cursor &where, std::string_view code,
                         std::string_view stage, std::string_view message);
void check_text(const Cursor &where, std::string_view value, bool required = false,
                std::size_t max_scalars = 4'096);
void check_id(const Cursor &where, std::string_view value);
void check_shape(const Document &input);
ResolvedTopology check_graph(const Document &input);

}
#endif
