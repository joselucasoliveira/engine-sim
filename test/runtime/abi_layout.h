#ifndef ENGINE_SIM_TEST_ABI_LAYOUT_H
#define ENGINE_SIM_TEST_ABI_LAYOUT_H

#include <float.h>
#include <limits.h>
#include <stddef.h>

#if defined(__cplusplus)
# define ABI_CHECK(test) static_assert(test, #test)
# define ABI_ALIGN(type) alignof(type)
#else
# define ABI_CHECK(test) _Static_assert(test, #test)
# define ABI_ALIGN(type) _Alignof(type)
#endif

#if defined(_WIN32)
# define ABI_CALL __cdecl
#else
# define ABI_CALL
#endif

ABI_CHECK(CHAR_BIT == 8 && sizeof(void *) == 8);
ABI_CHECK(sizeof(float) == 4 && sizeof(double) == 8);
ABI_CHECK(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128);
ABI_CHECK(DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024);
ABI_CHECK(sizeof(es_status) == 4 && (es_status)-1 < 0);
ABI_CHECK(sizeof(es_error) == 512 && ABI_ALIGN(es_error) == 1);
ABI_CHECK(offsetof(es_error, message) == 0);

ABI_CHECK(sizeof(es_config) == 40 && ABI_ALIGN(es_config) == 8);
ABI_CHECK(offsetof(es_config, size) == 0 && offsetof(es_config, abi) == 4);
ABI_CHECK(offsetof(es_config, output_hz) == 8 && offsetof(es_config, mode) == 12);
ABI_CHECK(offsetof(es_config, seed) == 16);
ABI_CHECK(offsetof(es_config, simulation_hz) == 24 && offsetof(es_config, flags) == 28);
ABI_CHECK(offsetof(es_config, starter_torque_nm) == 32);

ABI_CHECK(sizeof(es_controls) == 40 && ABI_ALIGN(es_controls) == 8);
ABI_CHECK(offsetof(es_controls, size) == 0 && offsetof(es_controls, abi) == 4);
ABI_CHECK(offsetof(es_controls, ignition) == 8 && offsetof(es_controls, starter) == 12);
ABI_CHECK(offsetof(es_controls, demand) == 16 && offsetof(es_controls, load_nm) == 24);
ABI_CHECK(offsetof(es_controls, driven_rpm) == 32);

ABI_CHECK(sizeof(es_telemetry) == 136 && ABI_ALIGN(es_telemetry) == 8);
ABI_CHECK(offsetof(es_telemetry, size) == 0 && offsetof(es_telemetry, abi) == 4);
ABI_CHECK(offsetof(es_telemetry, state) == 8 && offsetof(es_telemetry, mode) == 12);
ABI_CHECK(offsetof(es_telemetry, output_hz) == 16 && offsetof(es_telemetry, simulation_hz) == 20);
ABI_CHECK(offsetof(es_telemetry, cylinders) == 24 && offsetof(es_telemetry, reserved) == 28);
ABI_CHECK(offsetof(es_telemetry, frames) == 32 && offsetof(es_telemetry, ticks) == 40);
ABI_CHECK(offsetof(es_telemetry, native_frames) == 48 && offsetof(es_telemetry, measured_rpm) == 56);
ABI_CHECK(offsetof(es_telemetry, crank_rad_s) == 64 && offsetof(es_telemetry, cycle_rad) == 72);
ABI_CHECK(offsetof(es_telemetry, plate_angle_rad) == 80 && offsetof(es_telemetry, requested_load_nm) == 88);
ABI_CHECK(offsetof(es_telemetry, load_reaction_nm) == 96 && offsetof(es_telemetry, drive_reaction_nm) == 104);
ABI_CHECK(offsetof(es_telemetry, applied_driven_rpm) == 112 && offsetof(es_telemetry, driven_limit_rpm) == 120);
ABI_CHECK(offsetof(es_telemetry, displacement_m3) == 128);

ABI_CHECK(sizeof(es_diagnostic) == 2888 && ABI_ALIGN(es_diagnostic) == 8);
ABI_CHECK(offsetof(es_diagnostic, size) == 0 && offsetof(es_diagnostic, abi) == 4);
ABI_CHECK(offsetof(es_diagnostic, stage) == 8 && offsetof(es_diagnostic, truncated) == 12);
ABI_CHECK(offsetof(es_diagnostic, byte_offset) == 16 && offsetof(es_diagnostic, line) == 24);
ABI_CHECK(offsetof(es_diagnostic, column) == 32 && offsetof(es_diagnostic, code) == 40);
ABI_CHECK(offsetof(es_diagnostic, pointer) == 72 && offsetof(es_diagnostic, component_id) == 1096);
ABI_CHECK(offsetof(es_diagnostic, field) == 1224 && offsetof(es_diagnostic, path) == 1352);
ABI_CHECK(offsetof(es_diagnostic, message) == 2376);

ABI_CHECK(ES_ABI_V2 == 0x00020000u && ES_MAX_FRAMES == 65536u);
ABI_CHECK(ES_FREE == 0u && ES_DRIVEN == 1u);
ABI_CHECK(ES_EMPTY == 0u && ES_READY == 1u && ES_FAULTED == 2u);
ABI_CHECK(ES_OK == 0 && ES_INVALID == 1 && ES_VERSION == 2 && ES_UNSUPPORTED == 3);
ABI_CHECK(ES_STATE == 4 && ES_RESOURCE == 6 && ES_MODEL == 7);
ABI_CHECK(ES_CAPACITY == 8 && ES_NO_MEMORY == 9 && ES_RENDER_FAULT == 10 && ES_INTERNAL == 11 && ES_CONFIG == 12);
ABI_CHECK(ES_CAP_FREE == 1u && ES_CAP_44100 == 2u && ES_CAP_48000 == 4u && ES_CAP_LOAD == 8u);
ABI_CHECK(ES_CAP_DRIVEN == 16u && ES_CAP_RESET == 32u && ES_CAP_PARALLEL_RENDER == 64u);

ABI_CHECK(ES_CAP_JSON_FILE == 128u && ES_CAP_JSON_BYTES == 256u && ES_CAP_CONFIG_DIAGNOSTIC == 512u);
ABI_CHECK(ES_DIAG_NONE == 0 && ES_DIAG_PARSE == 1 && ES_DIAG_SCHEMA == 2 && ES_DIAG_NORMALIZE == 3);
ABI_CHECK(ES_DIAG_SEMANTIC == 4 && ES_DIAG_RESOURCE == 5 && ES_DIAG_PREPARE == 6 && ES_DIAG_API == 7);
ABI_CHECK(ES_DIAG_NO_LOCATION == UINT64_MAX);
ABI_CHECK(ES_DIAG_TRUNC_POINTER == 1 && ES_DIAG_TRUNC_COMPONENT == 2 && ES_DIAG_TRUNC_FIELD == 4);
ABI_CHECK(ES_DIAG_TRUNC_PATH == 8 && ES_DIAG_TRUNC_MESSAGE == 16);

#endif
