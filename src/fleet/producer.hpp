#ifndef ENGINE_SIM_PRIVATE_FLEET_PRODUCER_HPP
#define ENGINE_SIM_PRIVATE_FLEET_PRODUCER_HPP

#include "engine_sim/fleet.h"

#include <atomic>
#include <thread>

namespace engine_sim::fleet {

enum class FleetPhase : std::uint32_t { initial, starting, running, stopped };

// One C starts and joins W. A only observes phase; it never wakes the producer.
struct Producer {
    std::atomic<FleetPhase> phase{FleetPhase::initial};
    std::atomic<std::uint64_t> generated{0};

    std::int32_t start(es_rt_fleet &fleet);
    void quiesce();

private:
    std::atomic<bool> stopping{false};
    std::thread worker;
};

}
#endif
