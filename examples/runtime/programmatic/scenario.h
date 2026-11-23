#ifndef ENGINE_SIM_EXAMPLE_RUNTIME_SCENARIO_H
#define ENGINE_SIM_EXAMPLE_RUNTIME_SCENARIO_H

#include <engine_sim/runtime.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static inline int example_config(const char *rate, const char *mode, es_config *config, es_error *error) {
    if (es_config_init(ES_ABI_V2, (uint32_t)sizeof(*config), config, error) != ES_OK) return 0;
    if (strcmp(rate, "44100") == 0) config->output_hz = 44100;
    else if (strcmp(rate, "48000") == 0) config->output_hz = 48000;
    else return 0;
    if (strcmp(mode, "free") == 0) config->mode = ES_FREE;
    else if (strcmp(mode, "driven") == 0) config->mode = ES_DRIVEN;
    else return 0;
    config->seed = 403;
    return 1;
}

static inline int example_render(es_runtime *runtime, const es_config *config, FILE *output, es_error *error) {
    es_controls controls;
    memset(&controls, 0, sizeof controls);
    controls.size = (uint32_t)sizeof controls;
    controls.abi = ES_ABI_V2;
    controls.ignition = 1;
    controls.starter = config->mode == ES_FREE;
    controls.demand = .25;
    controls.driven_rpm = config->mode == ES_DRIVEN ? 600 : 0;
    if (es_set_controls(runtime, &controls, error) != ES_OK) return 0;

    const uint32_t rate = config->output_hz;
    for (uint32_t position = 0; position < rate * 2;) {
        if (position == rate) {
            controls.starter = 0;
            controls.load_nm = config->mode == ES_DRIVEN ? 1 : 0;
            if (es_set_controls(runtime, &controls, error) != ES_OK) return 0;
        }
        const uint32_t boundary = position < rate ? rate : rate * 2;
        const uint32_t count = boundary - position < 257 ? boundary - position : 257;
        uint32_t written = 0;
        float buffer[257];
        if (es_render(runtime, count, buffer, 257, &written, error) != ES_OK || written != count) return 0;
        if (fwrite(buffer, sizeof(float), written, output) != written) return 0;
        position += written;
    }

    es_telemetry telemetry;
    memset(&telemetry, 0, sizeof telemetry);
    telemetry.size = (uint32_t)sizeof telemetry;
    telemetry.abi = ES_ABI_V2;
    if (es_get_telemetry(runtime, &telemetry, error) != ES_OK || telemetry.cylinders != 1 ||
        telemetry.frames != rate * 2 || telemetry.native_frames != 88200 || telemetry.measured_rpm < 100) return 0;
    if (es_reset(runtime, error) != ES_OK) return 0;
    es_telemetry reset = telemetry;
    if (es_get_telemetry(runtime, &reset, error) != ES_OK || reset.frames || reset.ticks || reset.native_frames) return 0;
    printf("{\"rate\":%u,\"mode\":%u,\"cylinders\":%u,\"displacement_m3\":%.17g,"
           "\"frames\":%" PRIu64 ",\"ticks\":%" PRIu64 ",\"native_frames\":%" PRIu64 ",\"reset\":true}\n",
           rate, config->mode, telemetry.cylinders, telemetry.displacement_m3, telemetry.frames, telemetry.ticks,
           telemetry.native_frames);
    return 1;
}

#endif
