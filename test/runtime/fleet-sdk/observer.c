#include <stddef.h>

extern void *__libc_malloc(size_t);
extern void __libc_free(void *);

static _Thread_local unsigned armed __attribute__((tls_model("initial-exec")));
static _Thread_local unsigned hits __attribute__((tls_model("initial-exec")));

void fleet_probe_arm(void) {
    hits = 0;
    armed = 1;
}

unsigned fleet_probe_end(void) {
    armed = 0;
    return hits;
}

void *malloc(size_t size) {
    if (armed) ++hits;
    return __libc_malloc(size);
}

void free(void *value) {
    if (armed) ++hits;
    __libc_free(value);
}
