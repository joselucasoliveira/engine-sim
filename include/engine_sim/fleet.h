#ifndef ENGINE_SIM_FLEET_H
#define ENGINE_SIM_FLEET_H
#include <stdint.h>
#if defined(_WIN32)
# define ES_RT_CALL __cdecl
# if defined(ES_FLEET_STATIC)
#  define ES_RT_API
# elif defined(ES_FLEET_BUILD)
#  define ES_RT_API __declspec(dllexport)
# else
#  define ES_RT_API __declspec(dllimport)
# endif
#elif defined(__GNUC__)
# define ES_RT_API __attribute__((visibility("default")))
# define ES_RT_CALL
#else
# define ES_RT_API
# define ES_RT_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define ES_RT_V1 UINT32_C(0x00010000)
#define ES_RT_NO_FRAME UINT64_MAX
#define ES_RT_DFLT_MS UINT32_MAX
enum { ES_RT_OK, ES_RT_INVALID, ES_RT_VERSION, ES_RT_CAPACITY, ES_RT_UNSUPPORTED,
       ES_RT_STATE, ES_RT_FULL, ES_RT_STALE, ES_RT_ORDER, ES_RT_LATE, ES_RT_BUSY,
       ES_RT_BROKEN, ES_RT_RESOURCE, ES_RT_MODEL, ES_RT_PENDING, ES_RT_CANCELED,
       ES_RT_INTERRUPTED, ES_RT_FAULT, ES_RT_EMPTY, ES_RT_MEMORY, ES_RT_INTERNAL };
enum { ES_RT_DIAG_NONE, ES_RT_DIAG_ABI, ES_RT_DIAG_PROFILE, ES_RT_DIAG_INPUT,
       ES_RT_DIAG_RESOURCE, ES_RT_DIAG_PREPARE };
enum { ES_RT_BALANCED=1, ES_RT_INTERACTIVE=2 };
enum { ES_RT_GAME=0, ES_RT_RAW=1 };
enum { ES_RT_ACTIVATE=1, ES_RT_VECTOR, ES_RT_GAIN, ES_RT_MASTER, ES_RT_MUTE,
       ES_RT_UNMUTE, ES_RT_PAUSE, ES_RT_RESUME, ES_RT_STOP, ES_RT_REPLACE };
enum { ES_RT_VACANT, ES_RT_ATTACK, ES_RT_PLAY, ES_RT_MUTING, ES_RT_MUTED,
       ES_RT_PAUSING, ES_RT_PAUSED, ES_RT_OVERLAP, ES_RT_STOPPING, ES_RT_FAILED };
enum { ES_RT_FREE=0, ES_RT_DRIVEN=1, ES_RT_EXPERIMENTAL=1 };
typedef struct es_rt_fleet es_rt_fleet;
typedef struct es_rt_id { uint64_t generation; uint32_t lane, reserved; } es_rt_id;
typedef struct es_rt_token { uint64_t epoch, generation; uint32_t cell, reserved; } es_rt_token;
typedef struct es_rt_vector { double demand, load_nm, driven_rpm; uint32_t ignition, starter; } es_rt_vector;
typedef struct es_rt_config {
    uint32_t size, abi, rate, profile, max_frames, lanes, flags, qualification_id;
    uint64_t epoch;
} es_rt_config;
typedef struct es_rt_voice_config {
    uint32_t size, abi, mode, simulation_hz, controls_policy, reserved;
    uint64_t seed;
    double starter_torque_nm;
    es_rt_vector initial;
} es_rt_voice_config;
typedef struct es_rt_diagnostic { uint32_t size, abi, stage, code; char message[512]; } es_rt_diagnostic;
typedef struct es_rt_event {
    uint32_t size, abi, kind, duration_ms;
    uint64_t epoch, sequence, frame;
    es_rt_id voice, next_voice;
    es_rt_token candidate;
    es_rt_vector physical;
    double gain;
} es_rt_event;
typedef struct es_rt_result {
    uint32_t size, abi, reason, kind;
    uint64_t sequence, epoch, requested, applied, ended;
    es_rt_id voice, next_voice;
} es_rt_result;
typedef struct es_rt_lane_info {
    es_rt_id voice;
    uint32_t state, fault;
    uint64_t frames, ticks, native_frames, last_sequence, requested_frame, last_latched_sequence, latch_frame, latch_tick;
    es_rt_vector requested, applied;
    double measured_rpm, cycle_rad, plate_angle_rad, load_reaction_nm, drive_reaction_nm;
    double gain, gate;
} es_rt_lane_info;
typedef struct es_rt_snapshot {
    uint32_t size, abi, lanes, reserved;
    uint64_t epoch, generated, dropped;
    es_rt_lane_info lane[8];
} es_rt_snapshot;
typedef struct es_rt_transport {
    uint32_t size, abi, low_frames, high_frames, storage_frames, broken, returned_cells, outstanding;
    uint64_t epoch, submitted, generated, sealed, queue_rejections, dropped_snapshots;
} es_rt_transport;
typedef struct es_rt_audio {
    uint32_t size, abi, frames, capacity;
    float *mix, *stem[8];
} es_rt_audio;
typedef struct es_rt_block {
    uint32_t size, abi, pcm_frames, fallback_frames;
    uint64_t epoch, first;
} es_rt_block;
#define ES_RT_DECL(r,n,a) ES_RT_API r ES_RT_CALL n a
ES_RT_DECL(uint32_t, es_rt_api_version, (void));
ES_RT_DECL(int32_t, es_rt_create, (const es_rt_config *, es_rt_fleet **, es_rt_diagnostic *));
ES_RT_DECL(int32_t, es_rt_prepare_file, (es_rt_fleet *, const es_rt_voice_config *, const char *entry, const char *root, es_rt_token *, es_rt_diagnostic *));
ES_RT_DECL(int32_t, es_rt_prepare_bytes, (es_rt_fleet *, const es_rt_voice_config *, const uint8_t *, uint64_t length, const char *root, es_rt_token *, es_rt_diagnostic *));
ES_RT_DECL(int32_t, es_rt_discard, (es_rt_fleet *, es_rt_token));
ES_RT_DECL(int32_t, es_rt_submit, (es_rt_fleet *, const es_rt_event *));
ES_RT_DECL(int32_t, es_rt_poll_result, (es_rt_fleet *, uint64_t sequence, es_rt_result *));
ES_RT_DECL(int32_t, es_rt_poll_snapshot, (es_rt_fleet *, es_rt_snapshot *));
ES_RT_DECL(int32_t, es_rt_get_transport, (es_rt_fleet *, es_rt_transport *));
ES_RT_DECL(int32_t, es_rt_start, (es_rt_fleet *, es_rt_diagnostic *));
/* One audio owner. Valid nonempty spans always initialize the selected planes.
 * ES_RT_BROKEN reports the supplied PCM prefix and positive-zero fallback.
 * The first fallback frame is block.first + block.pcm_frames. A valid call
 * advances the submitted position by both counts, even after a break. */
ES_RT_DECL(int32_t, es_rt_process, (es_rt_fleet *, const es_rt_audio *, es_rt_block *));
ES_RT_DECL(int32_t, es_rt_quiesce, (es_rt_fleet *));
ES_RT_DECL(int32_t, es_rt_collect, (es_rt_fleet *, es_rt_token *, es_rt_lane_info *));
ES_RT_DECL(void, es_rt_destroy, (es_rt_fleet *));
#undef ES_RT_DECL
#ifdef __cplusplus
}
#endif
#endif
