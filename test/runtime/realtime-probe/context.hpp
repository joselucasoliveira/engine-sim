#ifndef ENGINE_SIM_TEST_REALTIME_CONTEXT_HPP
#define ENGINE_SIM_TEST_REALTIME_CONTEXT_HPP

#include <pthread.h>

struct probe_context {
    void *storage = nullptr;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
    pthread_spinlock_t spin{};
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    pthread_t helper{};
    bool ready = false;
    bool unwound = false;
};
#endif
