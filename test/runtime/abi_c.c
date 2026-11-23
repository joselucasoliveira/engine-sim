#include <engine_sim/runtime.h>
#include <engine_sim/runtime.h>

#include "abi_layout.h"

#define ABI_FUNCTION(name, result, arguments) \
    ABI_CHECK(_Generic(&name, result (ABI_CALL *) arguments: 1, default: 0))

ABI_FUNCTION(es_api_version, uint32_t, (void));
ABI_FUNCTION(es_capabilities, uint32_t, (void));
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

int main(void) {
    const uint32_t endian_probe = UINT32_C(0x04030201);
    const unsigned char *bytes = (const unsigned char *)&endian_probe;
    return bytes[0] == 1 && bytes[1] == 2 && bytes[2] == 3 && bytes[3] == 4 ? 0 : 1;
}
