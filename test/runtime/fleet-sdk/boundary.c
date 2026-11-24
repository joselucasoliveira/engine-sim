#include <engine_sim/fleet.h>
#include <stdlib.h>

int32_t fleet_probe_boundary(es_rt_fleet *fleet, const es_rt_audio *audio, es_rt_block *out, int inject) {
    if (inject) {
        void *value = malloc(64);
        free(value);
    }
    return es_rt_process(fleet, audio, out);
}
