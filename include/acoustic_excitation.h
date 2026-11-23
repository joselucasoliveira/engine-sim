#ifndef ENGINE_SIM_ACOUSTIC_EXCITATION_H
#define ENGINE_SIM_ACOUSTIC_EXCITATION_H

#include <cstddef>

namespace engine_sim {
// A borrowed extent of legacy pressure-derived excitation in first-encounter
// exhaust index order. Prepared rest is zero. Consume the values within the
// call that exposes them, before the producer reuses its storage.
struct AcousticExcitation {
    const double *data = nullptr;
    std::size_t count = 0;
};
}

#endif
