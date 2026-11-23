#include "observe.hpp"

#include <cstdio>
#include <dlfcn.h>

namespace realtime_probe {
namespace {
// The test observer is linked at program start. A dynamic TLS resolver in
// a wrapper could allocate on this thread or hide the first real call.
thread_local __attribute__((tls_model("initial-exec"))) Record *current = nullptr;
}

Scope::Scope(Record &result) noexcept : m_previous(current) { current = &result; }
Scope::~Scope() { current = m_previous; }

void record(Operation op, std::size_t bytes, const void *caller) noexcept {
    if (current == nullptr) return;
    auto &hit = (*current)[static_cast<std::size_t>(op)];
    if (hit.count++ == 0) {
        hit.first_bytes = bytes;
        hit.first_caller = caller;
    }
}

void dump(const Record &result) {
    for (std::size_t i = 0; i < result.size(); ++i) {
        const Hit &hit = result[i];
        if (hit.count) {
            Dl_info module{};
            const bool located = dladdr(hit.first_caller, &module) != 0;
            const auto offset = reinterpret_cast<std::uintptr_t>(hit.first_caller)
                - reinterpret_cast<std::uintptr_t>(module.dli_fbase);
            std::printf("observed %s %llu bytes=%zu caller=%p module=%s offset=0x%llx\n", names[i],
                static_cast<unsigned long long>(hit.count), hit.first_bytes, hit.first_caller,
                located ? module.dli_fname : "<unknown>", static_cast<unsigned long long>(offset));
        }
    }
}
}
