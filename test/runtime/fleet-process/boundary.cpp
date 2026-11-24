#include "boundary.hpp"
#include "escape.hpp"

#include <cstdio>
#include <cstdlib>
#include <pthread.h>

#ifndef ES_PROCESS_BOUNDARY
#error Missing probe boundary name
#endif

extern "C" int ES_PROCESS_BOUNDARY(es_rt_fleet *fleet, const es_rt_audio *audio,
                                  es_rt_block *result, bool inject) {
    if (inject) {
        void *memory = std::malloc(64);
        escape(memory);
        std::free(memory);
        pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
        if (pthread_mutex_lock(&mutex) || pthread_mutex_unlock(&mutex)) std::abort();
        char format[16];
        std::snprintf(format, sizeof(format), "%d", static_cast<int>(audio->frames));
        escape(format);
    }
    return es_rt_process(fleet, audio, result);
}
