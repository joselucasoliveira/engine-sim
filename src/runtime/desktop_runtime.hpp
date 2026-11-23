#ifndef ENGINE_SIM_PRIVATE_DESKTOP_RUNTIME_HPP
#define ENGINE_SIM_PRIVATE_DESKTOP_RUNTIME_HPP

#include "runtime.hpp"
#include "../../include/desktop_engine_views.h"

namespace desktop {
// Private owner adapter. Device pacing and presentation stay in the application;
// every source sample is rendered by the same Runtime computation as the C API.
class RuntimeHost {
public:
    static constexpr std::uint32_t MaxRenderFrames = 4410;
    static std::unique_ptr<RuntimeHost> prepareModel(const std::filesystem::path &entry,
                                                     const std::filesystem::path &root);
    static std::unique_ptr<RuntimeHost> prepare(const es_script::PreparationContext &recipe);
    static std::unique_ptr<RuntimeHost> adopt(EngineDefinition definition);
    std::unique_ptr<RuntimeHost> resetCandidate() const;
    ~RuntimeHost();
    RuntimeHost(const RuntimeHost &) = delete;
    RuntimeHost &operator=(const RuntimeHost &) = delete;

    EngineViews &views() { return *m_views; }
    EngineCommands &commands() { return *m_commands; }
    const Simulator *simulator() const { return m_runtime.m_session->prepared.session->simulator.get(); }
    const ApplicationSettings &settings() const { return m_runtime.m_session->prepared.session->definition.applicationSettings; }
    engine_sim::PullEngine &pull() { return *m_runtime.m_session->pull; }
    const engine_sim::ClockCounts &counts() const { return m_runtime.m_session->pull->counts(); }
    const es_telemetry &telemetry() const { return m_runtime.telemetry(); }
    void render(float *output, std::uint32_t frames, std::uint32_t &written);
    void requestFrequency(std::uint32_t frequency) { pull().requestDesktopFrequency(frequency); }
    void beginPresentation();
    void endPresentation();
    void observePresentation();
    double renderCpuSeconds() const { return m_renderCpuSeconds; }
    std::uint64_t presentationTicks() const { return counts().ticks - m_firstTick; }
private:
    friend struct engine_sim::RuntimeTestAccess;
    RuntimeHost();
    static std::unique_ptr<RuntimeHost> fromPrepared(engine_sim::PreparedEngine prepared);
    static std::unique_ptr<RuntimeHost> fromSession(std::unique_ptr<EngineSession> session, engine_sim::Runtime::Recipe recipe);
    engine_sim::Runtime m_runtime;
    std::unique_ptr<EngineViews> m_views;
    std::unique_ptr<EngineCommands> m_commands;
    double m_renderCpuSeconds = 0;
    std::uint64_t m_firstTick = 0;
};
}
#endif
