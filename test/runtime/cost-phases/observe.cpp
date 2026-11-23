#include "observe.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <new>
#ifdef ES_COST_WRAP_PTHREAD
#include <pthread.h>
#endif

namespace cost {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto untracked = Phase::untracked;
thread_local Phase phase = untracked;
thread_local Sample counters;

struct alignas(std::max_align_t) Allocation {
    const std::size_t bytes;
    const Phase created_in;
};

Counts &row(Phase value) {
    return counters.phases[static_cast<std::size_t>(value)];
}
}

const Sample &sample() {
    return counters;
}

void reset_interval() {
    assert(phase == untracked);
    for (auto &counts : counters.phases) {
        counts.calls = counts.ns = counts.allocations = counts.allocated_bytes = counts.locks = 0;
    }
    counters.peak_live_bytes = counters.live_bytes;
    counters.swap_live_bytes = counters.swaps = 0;
}

void before_swap() {
    if (phase == Phase::activation) {
        counters.swap_live_bytes = counters.live_bytes;
        ++counters.swaps;
    }
}

Scope::Scope(Phase next) : m_previous(std::exchange(phase, next)), m_start(Clock::now()) {}

Scope::~Scope() {
    auto &counts = row(phase);
    ++counts.calls;
    counts.ns += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_start).count();
    phase = m_previous;
}

void *allocate(std::size_t bytes) {
    if (bytes > std::numeric_limits<std::size_t>::max() - sizeof(Allocation)) throw std::bad_alloc();
    const auto size = sizeof(Allocation) + std::max<std::size_t>(bytes, 1);
    auto *storage = std::malloc(size);
    while (!storage) {
        auto handler = std::get_new_handler();
        if (!handler) throw std::bad_alloc();
        handler();
        storage = std::malloc(size);
    }
    auto *allocation = ::new (storage) Allocation{bytes, phase};
    if (phase != untracked) {
        auto &counts = row(phase);
        ++counts.allocations;
        ++counts.live_objects;
        counts.allocated_bytes += bytes;
        counts.live_bytes += bytes;
        ++counters.live_objects;
        counters.live_bytes += bytes;
        counters.peak_live_bytes = std::max(counters.peak_live_bytes, counters.live_bytes);
    }
    return reinterpret_cast<unsigned char *>(allocation) + sizeof(Allocation);
}

void release(void *pointer) noexcept {
    if (!pointer) return;
    auto *allocation = reinterpret_cast<Allocation *>(static_cast<unsigned char *>(pointer) - sizeof(Allocation));
    const auto origin = allocation->created_in;
    if (origin != untracked) {
        auto &counts = row(origin);
        if (!counts.live_objects || counts.live_bytes < allocation->bytes) std::abort();
        --counts.live_objects;
        counts.live_bytes -= allocation->bytes;
        --counters.live_objects;
        counters.live_bytes -= allocation->bytes;
    }
    std::free(allocation);
}

#ifdef ES_COST_WRAP_PTHREAD
void observe_lock() {
    if (phase != untracked) ++row(phase).locks;
}
#endif
}

void *operator new(std::size_t bytes) { return cost::allocate(bytes); }
void *operator new[](std::size_t bytes) { return cost::allocate(bytes); }
void operator delete(void *pointer) noexcept { cost::release(pointer); }
void operator delete[](void *pointer) noexcept { cost::release(pointer); }
void operator delete(void *pointer, std::size_t) noexcept { cost::release(pointer); }
void operator delete[](void *pointer, std::size_t) noexcept { cost::release(pointer); }

#ifdef ES_COST_WRAP_PTHREAD
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t *);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
    cost::observe_lock();
    return __real_pthread_mutex_lock(mutex);
}
#endif
