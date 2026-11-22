#include "desktop_runtime.hpp"
#include "../../include/simulation_fault.h"
#include <chrono>

namespace desktop {
RuntimeHost::RuntimeHost()
    : m_runtime(es_config{sizeof(es_config), ES_ABI_V1, 44100, ES_FREE, 1, 0, 0, 0},
        engine_sim::SampleClock::Profile::Desktop) {}
RuntimeHost::~RuntimeHost() {
    if (m_views) m_views->invalidate();
    m_commands.reset(); m_views.reset();
    m_runtime.unload();
}
std::unique_ptr<RuntimeHost> RuntimeHost::fromPrepared(engine_sim::PreparedEngine prepared) {
    auto result = std::unique_ptr<RuntimeHost>(new RuntimeHost);
    auto &simulator = *prepared.session->simulator;
    result->m_views = std::make_unique<EngineViews>(simulator);
    result->m_commands = std::make_unique<EngineCommands>(simulator, *result->m_views);
    auto *commands = result->m_commands.get();
    result->m_runtime.adoptPrepared(std::move(prepared), [commands] { commands->latch(); });
    result->pull().setTickCommands([commands] { commands->apply(commands->applied, false); },
        [commands] { commands->apply(commands->pending, true); });
    auto *views = result->m_views.get();
    result->pull().setTickObserver([views](const Simulator &sim) { views->sampleTick(sim); });
    return result;
}
std::unique_ptr<RuntimeHost> RuntimeHost::prepare(const es_script::PreparationContext &recipe) {
    engine_sim::PreparationConfig config;
    config.desktop = true;
    return fromPrepared(engine_sim::prepareEngine(recipe, config));
}
std::unique_ptr<RuntimeHost> RuntimeHost::adopt(EngineDefinition definition) {
    return fromPrepared(engine_sim::prepareDesktopEngine(std::move(definition)));
}
std::unique_ptr<RuntimeHost> RuntimeHost::resetCandidate() const {
    m_views->requireQuiescent();
    const auto &saved = m_runtime.m_session->prepared;
    if (saved.recipe.entry.empty() || saved.recipe.libraryRoot.empty() || saved.recipe.assetRoot.empty()) throw EngineLoadError({"unsupported", "", "Reset needs an owned script recipe"});
    return fromPrepared(engine_sim::prepareEngine(saved.recipe, saved.config, &saved.inputs));
}
void RuntimeHost::render(float *output, std::uint32_t frames, std::uint32_t &written) {
    m_views->requireQuiescent();
    if (frames > MaxRenderFrames) throw std::length_error("Desktop source quantum exceeds 100 ms");
    if (written != 0 || (frames && !output)) throw std::invalid_argument("Desktop source needs fresh bounded output storage");
    if (m_runtime.telemetry().state != ES_READY) throw engine_sim::SimulationFault("Desktop runtime is faulted");
    if (!frames) return;
    const auto start = std::chrono::steady_clock::now();
    struct Timing {
        double &seconds;
        std::chrono::steady_clock::time_point start;
        ~Timing() { seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }
    } timing{m_renderCpuSeconds, start};
    m_runtime.render(output, frames, written);
}
void RuntimeHost::beginPresentation() {
    m_views->requireQuiescent();
    m_firstTick = counts().ticks; m_renderCpuSeconds = 0;
    pull().beginPresentation();
}
void RuntimeHost::endPresentation() { m_views->requireQuiescent(); pull().endPresentation(); }
void RuntimeHost::observePresentation() {
    m_views->collectIgnition(*m_runtime.m_session->prepared.session->simulator);
}
}
