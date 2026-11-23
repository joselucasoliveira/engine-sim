#ifndef ENGINE_SIM_RUNTIME_COST_OBSERVE_HPP
#define ENGINE_SIM_RUNTIME_COST_OBSERVE_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace cost {
enum class Phase : std::size_t {
    untracked, owner, preparation, document_read, document_copy, tokenize_parse, schema, shared_schema, document_projection,
    normalization, semantic_graph, assembly, definition, resource_io, resource_decode, geometry_solver, shared_filter, dsp,
    convolution, activation, render, count
};

constexpr std::array labels{
    "untracked", "owner", "preparation", "document_read", "document_copy", "tokenize_parse", "schema", "shared_schema",
    "document_projection", "normalization", "semantic_graph", "assembly", "definition", "resource_io",
    "resource_decode", "geometry_solver", "shared_filter", "dsp", "convolution", "activation", "render"
};
static_assert(labels.size() == static_cast<std::size_t>(Phase::count));

struct Counts {
    std::uint64_t calls = 0, ns = 0, allocations = 0, allocated_bytes = 0;
    std::uint64_t live_objects = 0, live_bytes = 0, locks = 0;
};

using Rows = std::array<Counts, labels.size()>;
struct Sample {
    Rows phases{};
    std::uint64_t live_objects = 0, live_bytes = 0, peak_live_bytes = 0;
    std::uint64_t swap_live_bytes = 0, swaps = 0;
};

const Sample &sample();
void reset_interval();
void before_swap();

class Scope {
public:
    explicit Scope(Phase phase);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    Phase m_previous;
    std::chrono::steady_clock::time_point m_start;
};

template<class Callable>
decltype(auto) observe(Phase phase, Callable &&call) {
    const Scope scope(phase);
    return std::forward<Callable>(call)();
}
}

#endif
