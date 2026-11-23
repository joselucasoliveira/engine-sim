#include "scenario.h"

#ifdef _WIN32
#include <stdlib.h>
#include <windows.h>
#endif

static int load_file(char **args, FILE *output) {
    es_error error = {{0}};
    es_config config;
    es_runtime *runtime = NULL;
    int result = 1;
    if (!output) return result;
    if (example_config(args[3], args[4], &config, &error) &&
        es_create(&config, &runtime, &error) == ES_OK &&
        es_load_json_file(runtime, args[1], args[2], NULL, &error) == ES_OK &&
        example_render(runtime, &config, output, &error)) result = 0;
    es_destroy(runtime);
    if (fclose(output) != 0) result = 1;
    if (result) fprintf(stderr, "File render failed. %s\n", error.message);
    return result;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **wide) {
    char *args[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
    int result = 2;
    if (argc != 6) return result;
    for (int i = 1; i <= 4; ++i) {
        const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, NULL, 0, NULL, NULL);
        if (!length) goto done;
        args[i] = (char *)malloc((size_t)length);
        if (!args[i] || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, args[i], length, NULL, NULL)) goto done;
    }
    result = load_file(args, _wfopen(wide[5], L"wb"));
done:
    for (int i = 1; i <= 4; ++i) free(args[i]);
    return result;
}
#else
int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "Usage: engine-sim-file-loader INPUT.json ASSET_ROOT RATE MODE OUTPUT.f32\n");
        return 2;
    }
    return load_file(argv, fopen(argv[5], "wb"));
}
#endif
