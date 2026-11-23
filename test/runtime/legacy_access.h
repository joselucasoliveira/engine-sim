#ifndef ENGINE_SIM_TEST_LEGACY_ACCESS_H
#define ENGINE_SIM_TEST_LEGACY_ACCESS_H
#include "simulator.h"
#include <stdexcept>
namespace engine_sim_test {
// Only primitive regression tests use the independent legacy block scheduler.
struct LegacyAccess {
    static void begin(Simulator &simulator, int ticks) {
        if (ticks <= 0) throw std::invalid_argument("Legacy test needs positive ticks");
        if (!simulator.beginFrame()) return;
        simulator.m_steps = ticks;
        simulator.resetFrameFlows();
    }
    static void attach(Simulator &simulator, Synthesizer &synthesis) { simulator.m_legacySynthesis = &synthesis; }
};
}
#endif
