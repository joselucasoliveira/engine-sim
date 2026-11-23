#include <engine_sim/runtime.h>
#include <engine_sim/runtime.h>

#include "abi_layout.h"

#include <limits>
#include <type_traits>

ABI_CHECK(std::is_standard_layout_v<es_diagnostic> && std::is_trivially_copyable_v<es_diagnostic>);
ABI_CHECK(std::is_standard_layout_v<es_config> && std::is_trivially_copyable_v<es_config>);
ABI_CHECK(std::is_standard_layout_v<es_controls> && std::is_trivially_copyable_v<es_controls>);
ABI_CHECK(std::is_standard_layout_v<es_telemetry> && std::is_trivially_copyable_v<es_telemetry>);
ABI_CHECK(std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559);

#define ABI_FUNCTION(name, result, arguments) \
    ABI_CHECK((std::is_same_v<decltype(&name), result (ABI_CALL *) arguments>))

ABI_FUNCTION(es_api_version, uint32_t, ());
ABI_FUNCTION(es_capabilities, uint32_t, ());
ABI_FUNCTION(es_config_init, es_status, (uint32_t, uint32_t, es_config *, es_error *));
ABI_FUNCTION(es_create, es_status, (const es_config *, es_runtime **, es_error *));
ABI_FUNCTION(es_load_json_file, es_status, (es_runtime *, const char *, const char *, es_diagnostic *, es_error *));
ABI_FUNCTION(es_load_json_bytes, es_status, (es_runtime *, const uint8_t *, uint64_t, const char *, es_diagnostic *, es_error *));
ABI_FUNCTION(es_set_controls, es_status, (es_runtime *, const es_controls *, es_error *));
ABI_FUNCTION(es_render, es_status, (es_runtime *, uint32_t, float *, uint32_t, uint32_t *, es_error *));
ABI_FUNCTION(es_get_telemetry, es_status, (const es_runtime *, es_telemetry *, es_error *));
ABI_FUNCTION(es_reset, es_status, (es_runtime *, es_error *));
ABI_FUNCTION(es_reset_detailed, es_status, (es_runtime *, es_diagnostic *, es_error *));
ABI_FUNCTION(es_unload, es_status, (es_runtime *, es_error *));
ABI_FUNCTION(es_destroy, void, (es_runtime *));

int main() {}
