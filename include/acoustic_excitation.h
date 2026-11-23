#ifndef ENGINE_SIM_ACOUSTIC_EXCITATION_H
#define ENGINE_SIM_ACOUSTIC_EXCITATION_H

#include <cstddef>

namespace engine_sim {
// Legacy pressure-derived excitation in first-encounter exhaust index order.
// At prepared rest all channels are zero. The simulator owns the values until
// its next physics tick or teardown. Copy them during the current tick.
struct AcousticExcitation {
    const double *data;
    std::size_t count;
};
}

#endif
