#include "kernel.h"
#include "context.hpp"
#include "observe.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
constexpr auto alignment = std::align_val_t{64};

void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

void *signal_condition(void *arg) {
    auto &context = *static_cast<probe_context *>(arg);
    if (pthread_mutex_lock(&context.mutex)) std::abort();
    context.ready = true;
    if (pthread_cond_signal(&context.condition) || pthread_mutex_unlock(&context.mutex)) std::abort();
    return nullptr;
}

void *finish(void *) { return nullptr; }

int run(const char *control) {
    probe_context context;
    const auto is = [control](const char *name) { return std::strcmp(control, name) == 0; };
    const bool safe = is("safe");
    const bool waits = is("condition_wait") || is("condition_timed_wait");
    const bool array = std::strstr(control, "array") != nullptr;
    const bool aligned = std::strstr(control, "aligned") != nullptr;
    const bool deletes = std::strstr(control, "delete") != nullptr;
    if (deletes) {
        if (aligned) context.storage = array ? ::operator new[](128, alignment) : ::operator new(128, alignment);
        else context.storage = array ? ::operator new[](128) : ::operator new(128);
    } else if (is("free") || is("realloc")) {
        context.storage = std::malloc(128);
    } else if (is("aligned_free")) {
        context.storage = std::aligned_alloc(64, 128);
    }
    if (deletes || is("free") || is("aligned_free") || is("realloc")) {
        require(context.storage != nullptr, "control setup allocation");
    }
    if (is("recursive_lock")) {
        pthread_mutexattr_t config{};
        require(!pthread_mutexattr_init(&config), "recursive mutex attributes");
        require(!pthread_mutexattr_settype(&config, PTHREAD_MUTEX_RECURSIVE), "recursive mutex type");
        require(!pthread_mutex_destroy(&context.mutex), "retire ordinary mutex");
        require(!pthread_mutex_init(&context.mutex, &config), "recursive mutex setup");
        require(!pthread_mutexattr_destroy(&config), "retire mutex attributes");
        require(!pthread_mutex_lock(&context.mutex), "hold recursive mutex");
    }
    require(!pthread_spin_init(&context.spin, PTHREAD_PROCESS_PRIVATE), "spin setup");
    if (waits) require(!pthread_mutex_lock(&context.mutex), "hold wait mutex");
    if (is("condition_wait")) {
        require(!pthread_create(&context.helper, nullptr, signal_condition, &context), "condition helper");
    }
    if (is("join")) {
        require(!pthread_create(&context.helper, nullptr, finish, nullptr), "join helper");
    }

    const std::array<float, 2> input{0.25f, -0.75f};
    std::array<float, 4> out{42, 42, 42, 42};
    realtime_probe::Record hits{};
    int result = -1;
    std::printf("owner %zu control %s\n", std::hash<std::thread::id>{}(std::this_thread::get_id()), control);
    try {
        const realtime_probe::Scope measured(hits);
        result = probe_callback(safe ? nullptr : control, &context, out.data(), input.data());
    } catch (int value) {
        require(is("exception") && value == 403 && context.unwound, "exception failed to unwind through callee");
        result = 0;
    }
    realtime_probe::dump(hits);
    if (waits || is("recursive_lock")) require(!pthread_mutex_unlock(&context.mutex), "release setup mutex");
    if (is("condition_wait") || (is("thread_create") && result == 0)) {
        require(!pthread_join(context.helper, nullptr), "retire helper");
    }
    require(!pthread_spin_destroy(&context.spin), "retire spin");
    require(!pthread_cond_destroy(&context.condition), "retire condition");
    require(!pthread_rwlock_destroy(&context.rwlock), "retire rwlock");
    require(!pthread_mutex_destroy(&context.mutex), "retire mutex");
    if (!deletes && !is("free") && !is("aligned_free") && context.storage) {
        if (std::strstr(control, "new")) {
            if (aligned && array) ::operator delete[](context.storage, alignment);
            else if (aligned) ::operator delete(context.storage, alignment);
            else if (array) ::operator delete[](context.storage);
            else ::operator delete(context.storage);
        } else {
            std::free(context.storage);
        }
    }
    require(result == 0, "control call failed");
    if (safe) require(out == std::array<float, 4>{input[0], input[1], 0, 0}, "safe copy/zero mismatch");
    return 0;
}
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    int result = 1;
    std::thread owner([&] {
        try {
            result = run(argv[1]);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "%s: %s\n", argv[1], e.what());
        }
    });
    owner.join();
    return result;
}
