#include "observe.hpp"

#include <cstdarg>
#include <cstdio>
#include <new>
#include <pthread.h>
#include <unwind.h>

#define PROBE(symbol, result, parameters, arguments, bytes)                         \
    extern "C" result __real_##symbol parameters;                                   \
    extern "C" __attribute__((visibility("hidden"))) result __wrap_##symbol parameters { \
        realtime_probe::record(realtime_probe::Operation::symbol, bytes, __builtin_return_address(0)); \
        return __real_##symbol arguments;                                            \
    }
#include "operations.inc"
#undef PROBE

extern "C" __attribute__((visibility("hidden")))
int __wrap_snprintf(char *out, std::size_t capacity, const char *format, ...) {
    realtime_probe::record(realtime_probe::Operation::formatted_output, 0, __builtin_return_address(0));
    std::va_list args;
    va_start(args, format);
    const int result = std::vsnprintf(out, capacity, format, args);
    va_end(args);
    return result;
}
