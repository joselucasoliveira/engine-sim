#include "kernel.h"
#include "context.hpp"
#include "escape.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <time.h>

namespace {
constexpr auto alignment = std::align_val_t{64};

__attribute__((noinline)) int initialize() {
    const int result = 23;
    escape(&result);
    return result;
}

void *helper(void *) { return nullptr; }

struct UnwindWitness {
    bool &unwound;
    ~UnwindWitness() { unwound = true; }
};

__attribute__((noinline)) int exercise(const char *control, probe_context &context) {
    void *memory = nullptr;
#define CASE(name) if (std::strcmp(control, name) == 0)
#define ALLOC(name, call) CASE(name) { memory = call; context.storage = memory; escape(memory); return memory ? 0 : 1; }
    ALLOC("malloc", std::malloc(97))
    ALLOC("calloc", std::calloc(3, 97))
    CASE("realloc") {
        memory = std::realloc(context.storage, 191);
        escape(memory);
        if (memory) context.storage = memory;
        return memory ? 0 : 1;
    }
    ALLOC("aligned_alloc", std::aligned_alloc(64, 128))
    CASE("posix_memalign") return posix_memalign(&context.storage, 64, 128);

    ALLOC("new", ::operator new(97))
    ALLOC("array_new", ::operator new[](97))
    ALLOC("nothrow_new", ::operator new(97, std::nothrow))
    ALLOC("nothrow_array_new", ::operator new[](97, std::nothrow))
    ALLOC("aligned_new", ::operator new(128, alignment))
    ALLOC("aligned_array_new", ::operator new[](128, alignment))
    ALLOC("nothrow_aligned_new", ::operator new(128, alignment, std::nothrow))
    ALLOC("nothrow_aligned_array_new", ::operator new[](128, alignment, std::nothrow))
#undef ALLOC
#define RELEASE(name, call) CASE(name) { escape(context.storage); call; return 0; }
    RELEASE("free", std::free(context.storage))
    RELEASE("aligned_free", std::free(context.storage))
    RELEASE("delete", ::operator delete(context.storage))
    RELEASE("array_delete", ::operator delete[](context.storage))
    RELEASE("sized_delete", ::operator delete(context.storage, std::size_t{128}))
    RELEASE("sized_array_delete", ::operator delete[](context.storage, std::size_t{128}))
    RELEASE("aligned_delete", ::operator delete(context.storage, alignment))
    RELEASE("aligned_array_delete", ::operator delete[](context.storage, alignment))
    RELEASE("sized_aligned_delete", ::operator delete(context.storage, std::size_t{128}, alignment))
    RELEASE("sized_aligned_array_delete", ::operator delete[](context.storage, std::size_t{128}, alignment))
#undef RELEASE

    CASE("nested_allocation") return probe_nested_allocation();
    CASE("exception") {
        const UnwindWitness witness{context.unwound};
        throw 403;
    }
    CASE("lazy_init") {
        static const int value = initialize();
        return value == 23 ? 0 : 1;
    }
#define LOCK(name, acquire, release) CASE(name) { const int status = acquire; return status ? status : release; }
    LOCK("mutex_lock", pthread_mutex_lock(&context.mutex), pthread_mutex_unlock(&context.mutex))
    LOCK("recursive_lock", pthread_mutex_lock(&context.mutex), pthread_mutex_unlock(&context.mutex))
    LOCK("mutex_try", pthread_mutex_trylock(&context.mutex), pthread_mutex_unlock(&context.mutex))
    LOCK("read_lock", pthread_rwlock_rdlock(&context.rwlock), pthread_rwlock_unlock(&context.rwlock))
    LOCK("write_lock", pthread_rwlock_wrlock(&context.rwlock), pthread_rwlock_unlock(&context.rwlock))
    LOCK("read_try", pthread_rwlock_tryrdlock(&context.rwlock), pthread_rwlock_unlock(&context.rwlock))
    LOCK("write_try", pthread_rwlock_trywrlock(&context.rwlock), pthread_rwlock_unlock(&context.rwlock))
    LOCK("spin_lock", pthread_spin_lock(&context.spin), pthread_spin_unlock(&context.spin))
    LOCK("spin_try", pthread_spin_trylock(&context.spin), pthread_spin_unlock(&context.spin))
#undef LOCK
    CASE("condition_wait") {
        while (!context.ready) {
            const int status = pthread_cond_wait(&context.condition, &context.mutex);
            if (status) return status;
        }
        return 0;
    }
    CASE("condition_timed_wait") {
        const timespec elapsed{};
        return pthread_cond_timedwait(&context.condition, &context.mutex, &elapsed) == ETIMEDOUT ? 0 : 1;
    }
    CASE("sleep") {
        const timespec delay{0, 1000};
        return nanosleep(&delay, nullptr);
    }
    CASE("yield") return sched_yield();
    CASE("join") return pthread_join(context.helper, nullptr);
    CASE("thread_create") return pthread_create(&context.helper, nullptr, helper, nullptr);
    CASE("file_open") {
        FILE *file = std::fopen("/dev/null", "rb");
        return file ? std::fclose(file) : 1;
    }
    CASE("format") {
        char buffer[16]{};
        const int count = std::snprintf(buffer, sizeof(buffer), "%s", control);
        escape(buffer);
        return count == 6 ? 0 : 1;
    }
#undef CASE
    return 2;
}
}

extern "C" __attribute__((noinline))
int probe_callback(const char *control, probe_context *context, float *out, const float *in) {
    if (control) {
        const int result = exercise(control, *context);
        // Keep a return address in the callee even when a sentinel tail-calls its wrapper.
        escape(&result);
        return result;
    }
    out[0] = in[0];
    out[1] = in[1];
    out[2] = 0.0f;
    out[3] = 0.0f;
    return 0;
}
