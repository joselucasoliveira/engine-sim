#include "layout.h"
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); goto finish; \
} } while (0)

int fleet_c_consumer(const char *entry, const char *root) {
    struct { es_rt_config value; uint64_t tail; } config = {
        {0, ES_RT_V1, 48000, ES_RT_INTERACTIVE, 128, 2, ES_RT_EXPERIMENTAL, 0, 7}, UINT64_MAX};
    struct { es_rt_diagnostic value; uint64_t tail; } diagnostic = {{0}, UINT64_MAX};
    struct { es_rt_voice_config value; uint64_t tail; } voice = {
        {0, ES_RT_V1, ES_RT_FREE, 0, ES_RT_RAW, 0, 403, 0, {0.25, 0.375, 0, 1, 1}}, UINT64_MAX};
    const es_rt_token canary = {UINT64_MAX, UINT64_MAX, UINT32_MAX, UINT32_MAX};
    es_rt_token good = canary, token = canary;
    es_rt_fleet *fleet = NULL, *marker = NULL;
    int result = 1;
    config.value.size = sizeof(config);
    diagnostic.value.size = sizeof(diagnostic);
    diagnostic.value.abi = ES_RT_V1;
    voice.value.size = sizeof(voice);
    CHECK(es_rt_api_version() == ES_RT_V1);
    CHECK(es_rt_create(&config.value, &fleet, &diagnostic.value) == ES_RT_OK && fleet != NULL);
    CHECK(diagnostic.value.stage == ES_RT_DIAG_NONE && diagnostic.value.message[0] == '\0');
    CHECK(diagnostic.tail == UINT64_MAX && config.tail == UINT64_MAX);
    marker = fleet;
    config.value.flags = 0;
    CHECK(es_rt_create(&config.value, &marker, &diagnostic.value) == ES_RT_UNSUPPORTED && marker == fleet);
    CHECK(diagnostic.value.stage == ES_RT_DIAG_PROFILE && diagnostic.value.code == ES_RT_UNSUPPORTED);
    config.value.flags = ES_RT_EXPERIMENTAL;
    config.value.size = 8;
    CHECK(es_rt_create(&config.value, &marker, &diagnostic.value) == ES_RT_CAPACITY && marker == fleet);
    config.value.size = sizeof(config);
    config.value.abi = ES_RT_V1 + 1;
    CHECK(es_rt_create(&config.value, &marker, &diagnostic.value) == ES_RT_VERSION && marker == fleet);
    config.value.abi = ES_RT_V1;
    diagnostic.value.size = 8;
    CHECK(es_rt_create(&config.value, &marker, &diagnostic.value) == ES_RT_CAPACITY && marker == fleet);
    CHECK(diagnostic.value.code == ES_RT_VERSION && diagnostic.tail == UINT64_MAX);
    diagnostic.value.size = sizeof(diagnostic);
    CHECK(es_rt_prepare_file(fleet, &voice.value, entry, root, &good, &diagnostic.value) == ES_RT_OK);
    CHECK(good.epoch == 7 && good.generation != 0 && good.reserved == 0);
    CHECK(diagnostic.value.code == ES_RT_OK && diagnostic.tail == UINT64_MAX && voice.tail == UINT64_MAX);
    voice.value.reserved = 1;
    CHECK(es_rt_prepare_file(fleet, &voice.value, entry, root, &token, &diagnostic.value) == ES_RT_INVALID);
    CHECK(memcmp(&token, &canary, sizeof(token)) == 0);
    CHECK(es_rt_discard(fleet, good) == ES_RT_OK);
    CHECK(es_rt_discard(fleet, good) == ES_RT_STALE);
    result = 0;
finish:
    es_rt_destroy(fleet);
    return result;
}
