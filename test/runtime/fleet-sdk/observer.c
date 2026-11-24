#define _GNU_SOURCE
#include "observer.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

extern void *__libc_malloc(size_t);
extern void __libc_free(void *);
extern void *__libc_memalign(size_t, size_t);

static _Thread_local unsigned armed __attribute__((tls_model("initial-exec")));
static _Thread_local struct fleet_probe_hits hits __attribute__((tls_model("initial-exec")));

static int (*real_lock)(pthread_mutex_t *);
static int (*real_wait)(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
static void (*real_throw)(void *, void *, void (*)(void *));

int fleet_probe_setup(void) {
    real_lock = dlsym(RTLD_NEXT, "pthread_mutex_lock");
    real_wait = dlsym(RTLD_NEXT, "pthread_cond_timedwait");
    real_throw = dlsym(RTLD_NEXT, "__cxa_throw");
    return real_lock && real_wait && real_throw;
}

void fleet_probe_arm(void) {
    hits = (struct fleet_probe_hits){0};
    armed = 1;
}

void fleet_probe_end(struct fleet_probe_hits *out) {
    armed = 0;
    *out = hits;
}

void *malloc(size_t size) {
    if (armed) ++hits.count[PROBE_MALLOC];
    return __libc_malloc(size);
}

void free(void *value) {
    if (armed) ++hits.count[PROBE_FREE];
    __libc_free(value);
}

void *aligned_alloc(size_t alignment, size_t size) {
    if (armed) ++hits.count[PROBE_ALIGNED_ALLOC];
    return __libc_memalign(alignment, size);
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (armed) ++hits.count[PROBE_LOCK];
    int (*lock)(pthread_mutex_t *) = real_lock;
    if (!lock) lock = dlsym(RTLD_NEXT, "pthread_mutex_lock");
    return lock(mutex);
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *until) {
    if (armed) ++hits.count[PROBE_TIMED_WAIT];
    int (*wait)(pthread_cond_t *, pthread_mutex_t *, const struct timespec *) = real_wait;
    if (!wait) wait = dlsym(RTLD_NEXT, "pthread_cond_timedwait");
    return wait(cond, mutex, until);
}

__attribute__((noreturn)) void __cxa_throw(void *object, void *type, void (*destroy)(void *)) {
    if (armed) ++hits.count[PROBE_CXA_THROW];
    void (*thrower)(void *, void *, void (*)(void *)) = real_throw;
    if (!thrower) thrower = dlsym(RTLD_NEXT, "__cxa_throw");
    thrower(object, type, destroy);
    _Exit(127);
}
