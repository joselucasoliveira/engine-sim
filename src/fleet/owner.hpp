#ifndef ENGINE_SIM_PRIVATE_FLEET_OWNER_HPP
#define ENGINE_SIM_PRIVATE_FLEET_OWNER_HPP

#include "engine_sim/fleet.h"
#include "capacity.hpp"
#include "ingress.hpp"
#include "json_session.hpp"
#include "pcm.hpp"
#include "producer.hpp"
#include "runtime.hpp"
#include "snapshot.hpp"

#include <atomic>
#include <cmath>
#include <memory>

namespace engine_sim::fleet {

template<class Record>
std::int32_t prefix(const Record &value) noexcept {
    if (value.size < 2 * sizeof(std::uint32_t)) return ES_RT_CAPACITY;
    if (value.abi != ES_RT_V1) return ES_RT_VERSION;
    return value.size < sizeof(Record) ? ES_RT_CAPACITY : ES_RT_OK;
}

inline bool finiteRange(double number, double limit) noexcept {
    return std::isfinite(number) && number >= 0 && number <= limit;
}

inline bool validVector(const es_rt_vector &value) noexcept {
    return finiteRange(value.demand, 1) && finiteRange(value.load_nm, 10000) &&
        finiteRange(value.driven_rpm, 12000) && value.ignition <= 1 && value.starter <= 1;
}

struct TimelinePoint {
    enum class Kind { apply, latch } kind;
    std::uint64_t sequence, frame, localFrame, tick;
    es_rt_id voice;
};
using TimelineTrace = void (*)(void *, const TimelinePoint &) noexcept;

struct Voice {
    Voice(const es_config &config, const es_rt_voice_config &initial,
          assembly::FleetCapacity estimate, json_runtime::PreparedModel graph);
    void attachTimeline(es_rt_id voice, TimelineTrace observer, void *context) noexcept;
    void request(es_rt_vector vector, std::uint64_t sequence, std::uint64_t frame) noexcept;
    ProcessingStatus renderAt(std::uint64_t start, float *output, std::uint32_t count, std::uint32_t &written);
    es_rt_lane_info laneInfo(es_rt_id id, std::uint32_t state) const noexcept;

    Runtime runtime;
    assembly::FleetCapacity capacity;
    es_rt_vector initial;
    std::uint32_t controlsPolicy;
    std::uint32_t mode;
    double drivenLimitRpm = 0;

    es_rt_vector requested{}, applied{};
    std::uint32_t fault = ES_RT_OK;
    double gain = 1;
    std::uint64_t lastSequence = 0, requestedSequence = 0, requestedFrame = ES_RT_NO_FRAME;
    std::uint64_t latchedSequence = 0, latchFrame = ES_RT_NO_FRAME, latchTick = ES_RT_NO_FRAME;

private:
    es_rt_id timelineId{};
    TimelineTrace trace = nullptr;
    void *traceContext = nullptr;
    std::uint64_t renderOrigin = 0, localOrigin = 0;
};

enum class CellState : std::uint32_t { free, prepared, queued, active, returned };
struct Cell {
    std::atomic<CellState> state{CellState::free};
    std::atomic<std::uint64_t> generation{0};
    std::unique_ptr<Voice> voice;
    es_rt_id identity{};
    es_rt_lane_info finalInfo{};
};

void returnCell(Cell &cell) noexcept;

}

// The host gives one non-audio control owner exclusive access to this surface.
struct es_rt_fleet {
    es_rt_fleet(es_rt_config setup, std::size_t count)
        : config(setup), cellCount(count), cells(std::make_unique<engine_sim::fleet::Cell[]>(count)),
          pcm(engine_sim::fleet::PcmTransport::slotCount(setup)) {}

    es_rt_config config;
    std::size_t cellCount;
    std::unique_ptr<engine_sim::fleet::Cell[]> cells;
    std::size_t prepared = 0;
    engine_sim::fleet::CommandTransport commands;
    engine_sim::fleet::PcmTransport pcm;
    engine_sim::fleet::Producer producer;
    engine_sim::fleet::Snapshots snapshots;
};

namespace engine_sim::fleet {
inline Cell *preparedCell(es_rt_fleet &fleet, const es_rt_token &token) noexcept {
    if (token.reserved || token.epoch != fleet.config.epoch || !token.generation || token.cell >= fleet.cellCount)
        return nullptr;
    auto &cell = fleet.cells[token.cell];
    return cell.state.load(std::memory_order_acquire) == CellState::prepared &&
        cell.generation.load(std::memory_order_relaxed) == token.generation ? &cell : nullptr;
}
}

#endif
