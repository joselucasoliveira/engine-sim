/* C11. Run with absolute JSON entry and asset-root paths. */
#include <engine_sim/runtime.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>
#endif

static int run(int argc, char **argv) {
    es_error error = {{0}};
    es_config config;
    es_runtime *rt = NULL;
    es_controls controls = { sizeof(es_controls), ES_ABI_V2, 1, 1, 0.25, 0, 0 };
    es_telemetry telemetry = {0};
    float block[257];
    double energy = 0;
    int result = 1;
    if (argc != 3) return 2;
    if (es_config_init(ES_ABI_V2, sizeof(config), &config, &error) != ES_OK) goto done;
    config.seed = 1;
    if (es_create(&config, &rt, &error) != ES_OK ||
        es_load_json_file(rt, argv[1], argv[2], NULL, &error) != ES_OK ||
        es_set_controls(rt, &controls, &error) != ES_OK) goto done;
    for (uint32_t pos = 0; pos < 88200;) {
        uint32_t written = 0, count = 88200 - pos;
        if (pos == 44100) {
            controls.starter = 0;
            if (es_set_controls(rt, &controls, &error) != ES_OK) goto done;
        }
        if (pos < 44100 && count > 44100 - pos) count = 44100 - pos;
        if (count > 257) count = 257;
        if (es_render(rt, count, block, 257, &written, &error) != ES_OK || written != count) goto done;
        for (uint32_t i = 0; i < written; ++i)
            energy += (double)block[i] * block[i];
        pos += written;
    }
    telemetry.size = sizeof telemetry;
    telemetry.abi = ES_ABI_V2;
    if (es_get_telemetry(rt, &telemetry, &error) != ES_OK) goto done;
    printf("%" PRIu64 " frames, %.2f RPM, energy %.6g\n", telemetry.frames, telemetry.measured_rpm, energy);
    result = (telemetry.frames == 88200 && isfinite(energy) && energy > 0) ? 0 : 1;
done:
    if (result) fprintf(stderr, "Consumer failed. %s\n", error.message);
    es_destroy(rt);
    return result;
}

#ifdef _WIN32
/* MSVC x64 native host adapter. The library still receives ordinary UTF-8. */
int wmain(int argc, wchar_t **wide) {
    char *paths[3] = {NULL, NULL, NULL};
    int result = 2;
    if (argc != 3) return result;
    for (int i = 1; i < 3; ++i) {
        int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1,
                                        NULL, 0, NULL, NULL);
        if (bytes <= 0) goto release;
        paths[i] = (char *)malloc((size_t)bytes);
        if (!paths[i] || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1,
                                              paths[i], bytes, NULL, NULL)) goto release;
    }
    result = run(argc, paths);
release:
    for (int i = 1; i < 3; ++i) free(paths[i]);
    return result;
}
#else
int main(int argc, char **argv) { return run(argc, argv); }
#endif
