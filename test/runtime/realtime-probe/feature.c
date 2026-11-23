#include <stdlib.h>

#ifndef __has_attribute
#define __has_attribute(name) 0
#endif
#if !__has_attribute(nonblocking)
#error nonblocking annotation unavailable
#endif

__attribute__((nonblocking, noinline))
int test_entry(int *out) {
#ifdef PROBE_ADVERSE
    void *memory = malloc(97);
    __asm__ volatile("" : : "g"(memory) : "memory");
    free(memory);
#endif
    *out = 403;
    return 0;
}

int main(void) {
    int result = 0;
    const int status = test_entry(&result);
    return status || result != 403;
}
