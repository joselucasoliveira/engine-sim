#ifndef ENGINE_SIM_SIMULATION_FAULT_H
#define ENGINE_SIM_SIMULATION_FAULT_H

#include <stdexcept>

namespace engine_sim {
class SimulationFault : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
}
#endif
