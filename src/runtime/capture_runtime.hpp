#ifndef ENGINE_SIM_PRIVATE_CAPTURE_RUNTIME_HPP
#define ENGINE_SIM_PRIVATE_CAPTURE_RUNTIME_HPP

#include "runtime.hpp"
#include <array>
#include <limits>

namespace engine_sim_capture {
struct Controls {
    double demand = 0, dynoSpeed = 0;
    bool starter = false, ignition = false, dyno = false;
};
struct Range {
    double min = std::numeric_limits<double>::infinity();
    double max = -std::numeric_limits<double>::infinity();
    bool valid = true;
    void add(double value);
};
struct TickSample {
    std::uint64_t tick = 0, lit = 0;
    double cycle = 0, speed = 0, demand = 0, plate = 0;
    bool starter = false, ignition = false;
};
struct Diagnostics {
    Range pressure, moles, volume, manifold;
    // 10 ms at the highest capture rate, plus a boundary tick.
    std::array<TickSample, 961> samples{};
    std::size_t ticks = 0;
};

// Development-only client of Runtime. The fixed observer copies bounded tick
// diagnostics; file writes and descriptor evaluation happen after render.
class RuntimeHost {
public:
    static std::unique_ptr<RuntimeHost> prepare(const es_script::PreparationContext &recipe,
        std::uint64_t seed, bool mechanics = false);
    const Engine &engine() const { return *simulator().getEngine(); }
    const Simulator &simulator() const { return *m_runtime.m_session->prepared.session->simulator; }
    Synthesizer::BufferStatistics synthesisStatistics() const {
        return m_runtime.m_session->prepared.session->synthesis.getBufferStatistics();
    }
    const std::vector<es_script::ScriptInput> &inputs() const { return std::get<engine_sim::Runtime::LegacyRecipe>(m_runtime.m_session->prepared.recipe).inputs; }
    const es_telemetry &telemetry() const { return m_runtime.telemetry(); }
    const Diagnostics &diagnostics() const { return m_diagnostics; }
    TickSample current() const;
    void setControls(Controls controls);
    void beginBlock();
    void render(float *output, std::uint32_t frames, std::uint32_t &written);
    void endBlock();
private:
    RuntimeHost();
    void latch();
    void observe(const Simulator &simulator);
    engine_sim::Runtime m_runtime;
    Controls m_controls, m_applied;
    Diagnostics m_diagnostics;
    bool m_mechanics = false, m_block = false;
    std::uint32_t m_blockFrames = 0;
    std::uint64_t m_observedTicks = 0;
};
}
#endif
