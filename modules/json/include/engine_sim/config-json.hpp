#ifndef ENGINE_SIM_CONFIG_JSON_HPP
#define ENGINE_SIM_CONFIG_JSON_HPP

#include <engine_sim/config-validation.hpp>

#include <string>
#include <string_view>

namespace engine_sim::config_v1 {

// Read one v1 JSON document, apply defaults, and validate the graph. The
// result owns its configuration or diagnostic independently of the input.
ModelResult read_json(std::string_view bytes);

// Serialize fixed v1 units and effective defaults in canonical wire order.
// The returned string owns its bytes and ends with one LF.
std::string write_json(const NormalizedDocument &config);

}

namespace engine_sim::config_json {

// Prepare either JSON format and return the effective, version-independent model.
config_v1::ModelResult read_model(std::string_view bytes);

// Preserve authored v2 content, including omitted defaults and explicit tables.
// Expanded v1 content continues to use the canonical v1 writer.
using RewriteResult = std::variant<std::string, config_v1::Diagnostic>;
RewriteResult rewrite(std::string_view bytes);

}
#endif
