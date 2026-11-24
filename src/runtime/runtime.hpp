#ifndef ENGINE_SIM_PRIVATE_RUNTIME_HPP
#define ENGINE_SIM_PRIVATE_RUNTIME_HPP

#include "engine_sim/runtime.h"
#include "engine_session.h"
#include "pull_engine.h"
#include <memory>

namespace desktop { class RuntimeHost; }
namespace engine_sim_capture { class RuntimeHost; }

namespace engine_sim {
struct RuntimeTestAccess;
struct LayoutFixtureAccess;
struct JsonRuntimeAccess;
struct JsonRecipe;
class DrivenShaft;
namespace fleet { struct Voice; }
class Runtime {
public:
    explicit Runtime(es_config config, SampleClock::Profile profile = SampleClock::Profile::Public) noexcept;
    void setControls(const es_controls &controls) noexcept;
    void render(float *output, std::uint32_t frames, std::uint32_t &written);
    ProcessingStatus renderChecked(float *output, std::uint32_t frames, std::uint32_t &written);
    ProcessingStatus processingFailure() const noexcept { return m_processingFailure; }
    void unload() noexcept;
    const es_telemetry &telemetry() const noexcept { return m_snapshot; }
private:
    friend struct RuntimeTestAccess;
    friend struct JsonRuntimeAccess;
    friend class desktop::RuntimeHost;
    friend class engine_sim_capture::RuntimeHost;
    friend struct LayoutFixtureAccess;
    friend struct fleet::Voice;
    using Recipe = std::shared_ptr<const JsonRecipe>;
    struct PreparedGraph {
        Recipe recipe;
        std::unique_ptr<EngineSession> session;
    };
    void adoptSession(std::unique_ptr<EngineSession> engine, Recipe recipe, std::uint32_t mode, bool desktop,
                      PullEngine::ControlLatch controls, const std::function<void()> &beforeCommit = {});
    struct Session {
        Session(std::unique_ptr<EngineSession> engine, Recipe recipe, std::uint32_t mode, bool desktop,
                std::uint32_t outputHz, SampleClock::Profile profile, PullEngine::ControlLatch controls);
        ~Session();
        void latch();
        PreparedGraph prepared;
        es_controls requested{sizeof(es_controls), ES_ABI_V2, 0, 0, 0, 0, 0};
        es_controls (*shapeControls)(void *) noexcept = nullptr;
        void *shapeContext = nullptr;
        std::unique_ptr<DrivenShaft> drive;
        std::unique_ptr<PullEngine> pull;
    };
    ProcessingStatus snapshot(const Session &session, es_telemetry &value) const noexcept;
    es_config m_config;
    SampleClock::Profile m_profile;
    es_telemetry m_snapshot{};
    ProcessingStatus m_processingFailure = ProcessingStatus::ok;
    std::unique_ptr<Session> m_session;
};
}
struct es_runtime {
    explicit es_runtime(es_config config) : implementation(config) {}
    engine_sim::Runtime implementation;
};
#endif
