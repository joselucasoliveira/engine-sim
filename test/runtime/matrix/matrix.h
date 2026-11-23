#include <engine_sim/runtime.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static int run_case(char **paths, uint32_t rate, uint32_t mode) {
    es_config config;
    es_error error;
    es_controls control;
    es_telemetry telemetry;
    es_runtime *handle = NULL;
    float pcm[257];
    uint32_t position = 0;
    uint64_t fingerprint = UINT64_C(14695981039346656037);
    double energy = 0;
    int result = 1;
    memset(&error, 0, sizeof error);
    memset(&control, 0, sizeof control);
    memset(&telemetry, 0, sizeof telemetry);
    if (es_api_version() != ES_ABI_V2 || (es_capabilities() & UINT32_C(127)) != UINT32_C(127)) goto done;
    if (es_config_init(ES_ABI_V2, (uint32_t)sizeof config, &config, &error) != ES_OK) goto done;
    config.output_hz = rate;
    config.mode = mode;
    config.seed = 17;
    if (es_create(&config, &handle, &error) != ES_OK || es_load_json_file(handle, paths[1], paths[2], NULL, &error) != ES_OK) goto done;
    control.size = (uint32_t)sizeof control;
    control.abi = ES_ABI_V2;
    control.ignition = 1;
    control.starter = mode == ES_FREE;
    control.demand = .25;
    control.driven_rpm = mode == ES_DRIVEN ? 600 : 0;
    if (es_set_controls(handle, &control, &error) != ES_OK) goto done;
    while (position < rate * 2) {
        uint32_t count = rate * 2 - position, written = 99;
        if (position == rate) {
            control.starter = 0;
            control.load_nm = mode == ES_DRIVEN ? 1 : 0;
            if (es_set_controls(handle, &control, &error) != ES_OK) goto done;
        }
        if (position < rate && count > rate - position) count = rate - position;
        if (count > 257) count = 257;
        if (es_render(handle, count, pcm, 257, &written, &error) != ES_OK || written != count) goto done;
        for (uint32_t i = 0; i < written; ++i) {
            unsigned char bits[sizeof(float)];
            if (!isfinite(pcm[i]) || pcm[i] < -1 || pcm[i] > 32767.0f / 32768) goto done;
            energy += (double)pcm[i] * pcm[i];
            memcpy(bits, pcm + i, sizeof bits);
            for (size_t j = 0; j < sizeof bits; ++j) { fingerprint ^= bits[j]; fingerprint *= UINT64_C(1099511628211); }
        }
        position += written;
    }
    telemetry.size = (uint32_t)sizeof telemetry;
    telemetry.abi = ES_ABI_V2;
    if (es_get_telemetry(handle, &telemetry, &error) != ES_OK || telemetry.state != ES_READY || telemetry.mode != mode ||
        telemetry.output_hz != rate || telemetry.frames != rate * 2 || telemetry.native_frames != 88200 ||
        telemetry.ticks != (uint64_t)telemetry.simulation_hz * 2 || !isfinite(energy) || energy <= 0 ||
        telemetry.measured_rpm < 100 || telemetry.crank_rad_s >= 0) goto done;
    if (mode == ES_DRIVEN && (fabs(telemetry.measured_rpm - 600) > .1 || telemetry.applied_driven_rpm != 600 ||
        telemetry.requested_load_nm != 1 || !isfinite(telemetry.drive_reaction_nm))) goto done;
    printf("{\"rate\":%u,\"mode\":%u,\"frames\":%" PRIu64 ",\"ticks\":%" PRIu64 ",\"native_frames\":%" PRIu64
        ",\"pcm_fnv1a\":\"%016" PRIx64 "\",\"rpm\":%.17g,\"energy\":%.17g}\n",
        rate, mode, telemetry.frames, telemetry.ticks, telemetry.native_frames, fingerprint, telemetry.measured_rpm, energy);
    if (es_reset(handle, &error) != ES_OK || es_get_telemetry(handle, &telemetry, &error) != ES_OK || telemetry.frames != 0 ||
        telemetry.ticks != 0 || telemetry.native_frames != 0 || es_unload(handle, &error) != ES_OK || es_unload(handle, &error) != ES_OK) goto done;
    result = 0;
done:
    if (result) fprintf(stderr, "Matrix rate=%u mode=%u failed: %s\n", rate, mode, error.message);
    es_destroy(handle);
    return result;
}
static int run(int argc, char **paths) {
    const uint32_t endian = 1;
    if (argc != 3 || sizeof(void *) != 8 || sizeof(float) != 4 || *(const unsigned char *)&endian != 1) return 2;
    for (uint32_t mode = ES_FREE; mode <= ES_DRIVEN; ++mode)
        for (unsigned rate = 0; rate < 2; ++rate)
            if (run_case(paths, rate ? 48000 : 44100, mode)) return 1;
    es_destroy(NULL);
    return 0;
}
#ifdef _WIN32
int wmain(int argc, wchar_t **wide) {
    char *paths[3] = {NULL, NULL, NULL};
    int result = 2;
    if (argc != 3) return result;
    for (int i = 1; i < 3; ++i) {
        int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, NULL, 0, NULL, NULL);
        if (bytes <= 0) goto release;
        paths[i] = (char *)malloc((size_t)bytes);
        if (!paths[i] || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, paths[i], bytes, NULL, NULL)) goto release;
    }
    result = run(argc, paths);
release:
    for (int i = 1; i < 3; ++i) free(paths[i]);
    return result;
}
#else
int main(int argc, char **paths) { return run(argc, paths); }
#endif
