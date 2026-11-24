#include <engine_sim/fleet.h>
#include "observer.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

int fleet_probe_exception(void);

int32_t fleet_probe_boundary(es_rt_fleet *fleet, const es_rt_audio *audio, es_rt_block *out,
                             enum fleet_probe_control control, void *release) {
    if (control == PROBE_C_HEAP) {
        void *value = malloc(64);
        if (!value) return -1;
        free(value);
    } else if (control == PROBE_RELEASE) {
        free(release);
    } else if (control == PROBE_ALIGNED) {
        void *value = aligned_alloc(64, 128);
        if (!value) return -1;
        free(value);
    } else if (control == PROBE_WAIT) {
        pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
        pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
        const struct timespec elapsed = {0, 0};
        if (pthread_mutex_lock(&mutex)) return -1;
        const int result = pthread_cond_timedwait(&cond, &mutex, &elapsed);
        if (pthread_mutex_unlock(&mutex) || result != ETIMEDOUT) return -1;
    } else if (control == PROBE_THROW && !fleet_probe_exception()) {
        return -1;
    }
    return es_rt_process(fleet, audio, out);
}
