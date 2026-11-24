#ifndef ENGINE_SIM_PRIVATE_FLEET_SCHEDULER_HPP
#define ENGINE_SIM_PRIVATE_FLEET_SCHEDULER_HPP

#include "owner.hpp"
#include "envelope.hpp"

namespace engine_sim::fleet {

struct ScheduledTile {
    std::uint64_t first = 0;
    std::array<std::array<float, quantum>, 8> stem{};
    std::array<double, quantum> master{};
};

struct ScheduledLane {
    es_rt_id id{};
    std::uint32_t cell = no_cell, oldCell = no_cell;
    std::uint32_t state = ES_RT_VACANT;
    Envelope gate{0}, gain{1}, blend{0};
    std::optional<Command> transition;
    bool muted = false;
    bool forcedStop = false;
    double stopSample = 0;

    std::uint64_t endpoint() const noexcept {
        return state == ES_RT_OVERLAP ? blend.endpoint() : gate.endpoint();
    }
};

// One serial W owns this timeline and its active voice borrows. The C-side
// command reservations are advisory and are never read by the scheduler.
class Scheduler {
public:
    using SealObserver = void (*)(void *) noexcept;
    explicit Scheduler(es_rt_fleet &source, TimelineTrace trace = nullptr, void *context = nullptr,
                       std::uint64_t start = 0) noexcept
        : fleet(source), observer(trace), traceContext(context), next(start) {}
    ~Scheduler();

    std::int32_t renderTile(ScheduledTile &output, SealObserver afterSnapshot = nullptr, void *context = nullptr);
    std::uint64_t generated() const noexcept { return next; }
    const ScheduledLane &lane(std::size_t index) const noexcept { return lanes[index]; }
    void snapshot(es_rt_snapshot &out) const noexcept;

private:
    void renderRange(ScheduledTile &output, std::uint32_t begin, std::uint32_t end);
    void renderSegment(ScheduledTile &output, std::uint32_t begin, std::uint32_t end);
    std::int32_t apply(const Command &command) noexcept;
    std::int32_t replace(ScheduledLane &lane, const Command &command) noexcept;
    void adopt(ScheduledLane &lane, Cell &cell, const es_rt_event &event) noexcept;
    std::int32_t transition(ScheduledLane &lane, const Command &command, std::uint32_t state,
                            double target, std::uint32_t frames) noexcept;
    void complete(ScheduledLane &lane, std::uint64_t frame) noexcept;
    void cancel(ScheduledLane &lane, std::uint32_t reason, std::uint64_t frame) noexcept;
    void observeApply(const es_rt_event &event, const Voice *voice, es_rt_id id) const noexcept;

    es_rt_fleet &fleet;
    TimelineTrace observer;
    void *traceContext;
    std::uint64_t next;
    std::array<ScheduledLane, 8> lanes{};
    Envelope master{0.5};
    std::array<float, quantum> source{}, previous{};
};

}
#endif
