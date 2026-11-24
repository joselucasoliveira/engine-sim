#include <engine_sim/fleet.h>
#include <engine_sim/runtime.h>
#include <stddef.h>
#include <stdio.h>

_Static_assert(sizeof(es_rt_config) == 40, "config");
_Static_assert(sizeof(es_rt_audio) == 88, "audio");
_Static_assert(sizeof(es_rt_block) == 32, "block");
_Static_assert(sizeof(es_rt_event) == 136, "event");
_Static_assert(offsetof(es_rt_audio, stem) == 24, "stem");

int main(void) {
    if (es_rt_api_version() != ES_RT_V1 || es_api_version() != ES_ABI_V2) return 1;
    es_config raw_config = {0};
    es_runtime *raw = NULL;
    if (es_config_init(ES_ABI_V2, sizeof raw_config, &raw_config, NULL) != ES_OK ||
        es_create(&raw_config, &raw, NULL) != ES_OK) return 2;
    es_rt_config options = {sizeof options, ES_RT_V1, 44100, ES_RT_BALANCED, 64, 1, ES_RT_EXPERIMENTAL, 0, 317};
    es_rt_fleet *fleet = NULL;
    options.flags |= 2;
    if (es_rt_create(&options, &fleet, NULL) != ES_RT_UNSUPPORTED || fleet) return 3;
    options.flags = ES_RT_EXPERIMENTAL;
    options.size -= 1;
    if (es_rt_create(&options, &fleet, NULL) != ES_RT_CAPACITY || fleet) return 4;
    options.size += 1;
    if (es_rt_create(&options, &fleet, NULL) != ES_RT_OK) return 5;
    if (es_rt_start(fleet, NULL) != ES_RT_OK) return 6;
    float sample = 77;
    es_rt_audio audio = {sizeof audio - 1, ES_RT_V1, 1, 1, &sample, {0}};
    struct extended {
        es_rt_block block;
        uint64_t tail;
    } result = {{sizeof result, ES_RT_V1, 33, 11, 9, 8}, UINT64_C(0xdecafbad)};
    if (es_rt_process(fleet, &audio, &result.block) != ES_RT_CAPACITY ||
        result.block.first != 8 || result.tail != UINT64_C(0xdecafbad) || sample != 77) return 7;
    audio.size += 1;
    if (es_rt_process(fleet, &audio, &result.block) != ES_RT_OK ||
        result.block.first != 0 || result.block.epoch != 317 || result.block.pcm_frames != 1 ||
        result.tail != UINT64_C(0xdecafbad) || sample != 0) return 8;
    if (es_rt_quiesce(fleet) != ES_RT_OK) return 9;
    es_rt_destroy(fleet);
    es_destroy(raw);
    puts("co-installed Runtime 2 and Fleet 1; ABI flags, sizes and output tails verified");
    return 0;
}
