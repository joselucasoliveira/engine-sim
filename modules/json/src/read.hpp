#ifndef ENGINE_SIM_JSON_READ_HPP
#define ENGINE_SIM_JSON_READ_HPP

#include <engine_sim/config-validation.hpp>

#include <string_view>

namespace engine_sim::config_v1::json_detail {

// Internal v1 registry used by configuration loading and migration tools.
std::string_view schema_bytes() noexcept;
std::string_view schema_digest() noexcept;
using AuthoredResult = std::variant<Document, Diagnostic>;
AuthoredResult read_authored(std::string_view input);
ModelResult read_normalized(std::string_view input);

struct ModelLimits {
    std::size_t document_bytes;
    std::size_t definitions;
    std::size_t points_per_curve;
    std::size_t points_total;
};
ModelResult read_limited_model(std::string_view input, const ModelLimits &limits);

}
#endif
