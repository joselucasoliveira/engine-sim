#include "desktop_runtime.hpp"
#include "json_session.hpp"
#include "../../include/simulation_fault.h"
#include <chrono>

namespace desktop {
namespace {
constexpr std::uint64_t desktopSeed = 1;

engine_sim::json_runtime::PreparedModel requireModel(engine_sim::json_runtime::PreparedModelResult result,
                                                      const std::filesystem::path &entry = {}) {
    if (const auto *error = std::get_if<engine_sim::config_v1::Diagnostic>(&result)) {
        auto location = error->code;
        if (!error->pointer.empty()) location += " " + error->pointer;
        if (error->line && error->column)
            location += " line " + std::to_string(*error->line) + ", column " + std::to_string(*error->column);
        throw EngineLoadError({error->stage, error->path.value_or(entry.u8string()), location + "\n" + error->message});
    }
    return std::get<engine_sim::json_runtime::PreparedModel>(std::move(result));
}
}
RuntimeHost::RuntimeHost()
    : m_runtime(es_config{sizeof(es_config), ES_ABI_V1, 44100, ES_FREE, desktopSeed, 0, 0, 0},
        engine_sim::SampleClock::Profile::Desktop) {}
RuntimeHost::~RuntimeHost() {
    if (m_views) m_views->invalidate();
    m_commands.reset(); m_views.reset();
    m_runtime.unload();
}
std::unique_ptr<RuntimeHost> RuntimeHost::fromPrepared(engine_sim::PreparedEngine prepared) {
    return fromSession(std::move(prepared.session),
        engine_sim::Runtime::LegacyRecipe{std::move(prepared.recipe), prepared.config, std::move(prepared.inputs)});
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
    auto prepared = requireModel(engine_sim::json_runtime::prepare_desktop(entry, root, desktopSeed), entry);
    return fromSession(std::move(prepared.session), std::move(prepared.recipe));
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
    if (const auto *saved = std::get_if<std::shared_ptr<const engine_sim::JsonRecipe>>(&m_runtime.m_session->prepared.recipe)) {
        auto prepared = requireModel(engine_sim::json_runtime::reset_desktop(**saved, desktopSeed));
        return fromSession(std::move(prepared.session), std::move(prepared.recipe));
    }
    const auto &saved = std::get<engine_sim::Runtime::LegacyRecipe>(m_runtime.m_session->prepared.recipe);
    if (saved.paths.entry.empty() || saved.paths.libraryRoot.empty() || saved.paths.assetRoot.empty()) throw EngineLoadError({"unsupported", "", "Reset needs an owned script recipe"});
    return fromPrepared(engine_sim::prepareEngine(saved.paths, saved.config, &saved.inputs));
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
