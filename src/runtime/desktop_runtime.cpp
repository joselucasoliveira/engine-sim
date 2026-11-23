#include "desktop_runtime.hpp"
#include "json_session.hpp"
#include "concrete_validation.hpp"
#include "wave_reader.h"
#include "../../include/simulation_fault.h"
#include <chrono>

namespace desktop {
namespace {
constexpr std::uint64_t desktopSeed = 1;

}
RuntimeHost::RuntimeHost()
    : m_runtime(es_config{sizeof(es_config), ES_ABI_V2, 44100, ES_FREE, desktopSeed, 0, 0, 0},
        engine_sim::SampleClock::Profile::Desktop) {}
RuntimeHost::~RuntimeHost() {
    if (m_views) m_views->invalidate();
    m_commands.reset(); m_views.reset();
    m_runtime.unload();
}
std::unique_ptr<RuntimeHost> RuntimeHost::fromSession(std::unique_ptr<EngineSession> session,
                                                       engine_sim::Runtime::Recipe recipe) {
    auto result = std::unique_ptr<RuntimeHost>(new RuntimeHost);
    auto &simulator = *session->simulator;
    auto &synthesis = session->synthesis;
    result->m_views = std::make_unique<EngineViews>(simulator, synthesis);
    result->m_commands = std::make_unique<EngineCommands>(simulator, synthesis, *result->m_views);
    auto *commands = result->m_commands.get();
    result->m_runtime.adoptSession(std::move(session), std::move(recipe), ES_FREE, true,
                                   [commands] { commands->latch(); });
    result->pull().setTickCommands([commands] { commands->apply(commands->applied, false); },
        [commands] { commands->apply(commands->pending, true); });
    auto *views = result->m_views.get();
    result->pull().setTickObserver([views](const Simulator &sim) { views->sampleTick(sim); });
    return result;
}
std::unique_ptr<RuntimeHost> RuntimeHost::prepareModel(const std::filesystem::path &entry,
                                                        const std::filesystem::path &root) {
    auto prepared = engine_sim::json_runtime::require_model(engine_sim::json_runtime::prepare_desktop(entry, root, desktopSeed), entry);
    return fromSession(std::move(prepared.session), std::move(prepared.recipe));
}
std::unique_ptr<RuntimeHost> RuntimeHost::adopt(EngineDefinition definition) {
    const auto entry = std::filesystem::u8path(definition.sourcePath);
    if (!definition.engine) throw EngineLoadError({"graph", definition.sourcePath, "No engine was selected"});
    auto &engine = *definition.engine;
    engine.setRandomSeed(engine.getRandomSeed());
    engine_sim::validateConcreteModel(engine, engine.getSimulationFrequency(), true, entry);
    auto session = EngineSession::prepare(std::move(definition), [](const ImpulseResponse &impulse) {
        const auto path = std::filesystem::u8path(impulse.getFilename());
        engine_sim::requireConcreteModel(std::isfinite(impulse.getVolume()) &&
            std::abs(impulse.getVolume()) <= std::numeric_limits<float>::max(), "impulse gain exceeds float range", path);
        try { return engine_sim::readImpulseWave(path); }
        catch (const engine_sim::WaveError &failure) {
            throw EngineLoadError({"resources", path.u8string(), failure.reason});
        }
    });
    return fromSession(std::move(session), {});
}
std::unique_ptr<RuntimeHost> RuntimeHost::resetCandidate() const {
    m_views->requireQuiescent();
    const auto &saved = m_runtime.m_session->prepared.recipe;
    if (!saved)
        throw EngineLoadError({"unsupported", "", "Reset needs an owned JSON recipe"});
    auto prepared = engine_sim::json_runtime::require_model(engine_sim::json_runtime::reset_desktop(*saved, desktopSeed));
    return fromSession(std::move(prepared.session), std::move(prepared.recipe));
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
