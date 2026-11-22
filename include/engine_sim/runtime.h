#ifndef ENGINE_SIM_RUNTIME_H
#define ENGINE_SIM_RUNTIME_H
#include <stdint.h>

#if defined(_WIN32)
# define ES_CALL __cdecl
# if defined(ES_RUNTIME_STATIC)
#  define ES_API
# elif defined(ES_RUNTIME_BUILD)
#  define ES_API __declspec(dllexport)
# else
#  define ES_API __declspec(dllimport)
# endif
#elif defined(__GNUC__)
# define ES_CALL
# define ES_API __attribute__((visibility("default")))
#else
# define ES_CALL
# define ES_API
#endif

#ifdef __cplusplus
extern "C" {
#endif
#define ES_ABI_V1 UINT32_C(0x00010000)
#define ES_MAX_FRAMES UINT32_C(65536)
#define ES_CAP_FREE    UINT32_C(1)
#define ES_CAP_44100   UINT32_C(2)
#define ES_CAP_48000   UINT32_C(4)
#define ES_CAP_LOAD    UINT32_C(8)
#define ES_CAP_DRIVEN  UINT32_C(16)
#define ES_CAP_RESET   UINT32_C(32)
#define ES_CAP_PARALLEL_RENDER UINT32_C(64)
#define ES_FREE UINT32_C(0)
#define ES_DRIVEN UINT32_C(1)
#define ES_EMPTY UINT32_C(0)
#define ES_READY UINT32_C(1)
#define ES_FAULTED UINT32_C(2)

typedef int32_t es_status;
#define ES_OK             INT32_C(0)
#define ES_INVALID        INT32_C(1)
#define ES_VERSION        INT32_C(2)
#define ES_UNSUPPORTED    INT32_C(3)
#define ES_STATE          INT32_C(4)
#define ES_SCRIPT         INT32_C(5)
#define ES_RESOURCE       INT32_C(6)
#define ES_MODEL          INT32_C(7)
#define ES_CAPACITY       INT32_C(8)
#define ES_NO_MEMORY      INT32_C(9)
#define ES_RENDER_FAULT   INT32_C(10)
#define ES_INTERNAL       INT32_C(11)

typedef struct es_runtime es_runtime;
/* Fixed storage, valid without initialization. Always NUL-terminated when supplied. */
typedef struct es_error { char message[512]; } es_error;
typedef struct es_config {
    uint32_t size, abi, output_hz, mode;
    uint64_t seed;
    uint32_t simulation_hz, flags;
    double starter_torque_nm;
} es_config;
typedef struct es_controls {
    uint32_t size, abi, ignition, starter;
    double demand, load_nm, driven_rpm;
} es_controls;
typedef struct es_telemetry {
    uint32_t size, abi, state, mode;
    uint32_t output_hz, simulation_hz, cylinders, reserved;
    uint64_t frames, ticks, native_frames;
    double measured_rpm, crank_rad_s, cycle_rad, plate_angle_rad;
    double requested_load_nm, load_reaction_nm, drive_reaction_nm;
    double applied_driven_rpm, driven_limit_rpm, displacement_m3;
} es_telemetry;

ES_API uint32_t ES_CALL es_api_version(void);
ES_API uint32_t ES_CALL es_capabilities(void);
ES_API es_status ES_CALL es_config_init(uint32_t abi, uint32_t capacity,
                                         es_config *config, es_error *error);
ES_API es_status ES_CALL es_create(const es_config *config, es_runtime **out, es_error *error);
ES_API es_status ES_CALL es_load(es_runtime *rt, const char *entry_utf8,
                                  const char *library_root_utf8, const char *asset_root_utf8,
                                  es_error *error);
ES_API es_status ES_CALL es_set_controls(es_runtime *rt, const es_controls *controls,
                                          es_error *error);
ES_API es_status ES_CALL es_render(es_runtime *rt, uint32_t frames, float *mono,
                                    uint32_t capacity_frames, uint32_t *written, es_error *error);
ES_API es_status ES_CALL es_get_telemetry(const es_runtime *rt, es_telemetry *out,
                                           es_error *error);
ES_API es_status ES_CALL es_reset(es_runtime *rt, es_error *error);
ES_API es_status ES_CALL es_unload(es_runtime *rt, es_error *error);
ES_API void ES_CALL es_destroy(es_runtime *rt);
#ifdef __cplusplus
}
#endif
#endif
