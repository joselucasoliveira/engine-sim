#ifndef ENGINE_SIM_PRIVATE_FLEET_OWNER_HPP
#define ENGINE_SIM_PRIVATE_FLEET_OWNER_HPP

#include "engine_sim/fleet.h"
#include "capacity.hpp"
#include "json_session.hpp"
#include "runtime.hpp"

#include <atomic>
#include <memory>

namespace engine_sim::fleet {

struct Voice {
    Voice(const es_config &config, const es_rt_voice_config &initial,
          assembly::FleetCapacity estimate, json_runtime::PreparedModel graph);

    Runtime runtime;
    assembly::FleetCapacity capacity;
    es_rt_vector initial;
    std::uint32_t controlsPolicy;
};

enum class CellState : std::uint32_t { free, prepared };
struct Cell {
    std::atomic<CellState> state{CellState::free};
    std::atomic<std::uint64_t> generation{0};
    std::unique_ptr<Voice> voice;
};

}

// The host gives one non-audio control owner exclusive access to this surface.
struct es_rt_fleet {
    es_rt_fleet(es_rt_config setup, std::size_t count)
        : config(setup), cellCount(count), cells(std::make_unique<engine_sim::fleet::Cell[]>(count)) {}

    es_rt_config config;
    std::size_t cellCount;
    std::unique_ptr<engine_sim::fleet::Cell[]> cells;
    std::size_t prepared = 0;
};

#endif
