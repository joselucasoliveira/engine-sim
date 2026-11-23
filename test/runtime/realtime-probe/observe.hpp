#ifndef ENGINE_SIM_TEST_REALTIME_OBSERVE_HPP
#define ENGINE_SIM_TEST_REALTIME_OBSERVE_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace realtime_probe {
enum class Operation {
#define PROBE(symbol, ...) symbol,
#include "operations.inc"
#undef PROBE
    formatted_output,
    count
};

inline constexpr std::array names{
#define PROBE(symbol, ...) #symbol,
#include "operations.inc"
#undef PROBE
    "snprintf"
};
static_assert(names.size() == static_cast<std::size_t>(Operation::count));

struct Hit {
    std::uint64_t count = 0;
    std::size_t first_bytes = 0;
    const void *first_caller = nullptr;
};
using Record = std::array<Hit, names.size()>;

void record(Operation op, std::size_t bytes, const void *caller) noexcept;
void dump(const Record &result);

class Scope {
public:
    explicit Scope(Record &result) noexcept;
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
private:
    Record *m_previous;
};
}
#endif
