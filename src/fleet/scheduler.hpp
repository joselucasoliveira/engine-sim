#ifndef ENGINE_SIM_PRIVATE_FLEET_SCHEDULER_HPP
#define ENGINE_SIM_PRIVATE_FLEET_SCHEDULER_HPP

#include "owner.hpp"

namespace engine_sim::fleet {

struct ScheduledTile {
    std::uint64_t first = 0;
    std::array<std::array<float, quantum>, 8> stem{};
};

struct ScheduledLane {
    es_rt_id id{};
    std::uint32_t cell = no_cell;
    double gain = 1;
    bool failed = false;
};

// One serial W owns this timeline and its active voice borrows. The C-side
// command reservations are advisory and are never read by the scheduler.
class Scheduler {
public:
    using SealObserver = void (*)(void *) noexcept;
    explicit Scheduler(es_rt_fleet &source, TimelineTrace trace = nullptr, void *context = nullptr,
                       std::uint64_t start = 0) noexcept
        : fleet(source), observer(trace), traceContext(context), next(start) {}

    std::int32_t renderTile(ScheduledTile &output, SealObserver afterSnapshot = nullptr, void *context = nullptr);
    std::uint64_t generated() const noexcept { return next; }
    const ScheduledLane &lane(std::size_t index) const noexcept { return lanes[index]; }
    double masterGain() const noexcept { return master; }

private:
    void renderRange(ScheduledTile &output, std::uint32_t begin, std::uint32_t end);
    std::int32_t apply(const es_rt_event &event) noexcept;
    void observeApply(const es_rt_event &event, const Voice *voice, es_rt_id id) const noexcept;

    es_rt_fleet &fleet;
    TimelineTrace observer;
    void *traceContext;
    std::uint64_t next;
    std::array<ScheduledLane, 8> lanes{};
    double master = 0.5;
};

}
#endif
