#ifndef ENGINE_SIM_PRIVATE_RUNTIME_HPP
#define ENGINE_SIM_PRIVATE_RUNTIME_HPP

#include "engine_sim/runtime.h"
#include "prepared_engine.h"
#include "pull_engine.h"
#include <memory>

namespace desktop { class RuntimeHost; }
namespace engine_sim_capture { class RuntimeHost; }

namespace engine_sim {
struct RuntimeTestAccess;
class DrivenShaft;
class Runtime {
public:
    explicit Runtime(es_config config, SampleClock::Profile profile = SampleClock::Profile::Public) noexcept;
    void load(const es_script::PreparationContext &recipe);
    void reset();
    void setControls(const es_controls &controls) noexcept;
    void render(float *output, std::uint32_t frames, std::uint32_t &written);
    void unload() noexcept;
    const es_telemetry &telemetry() const noexcept { return m_snapshot; }
private:
    friend struct RuntimeTestAccess;
    friend class desktop::RuntimeHost;
    friend class engine_sim_capture::RuntimeHost;
    void adoptPrepared(PreparedEngine prepared, PullEngine::ControlLatch controls = {});
    struct Session {
        Session(PreparedEngine value, std::uint32_t outputHz, SampleClock::Profile profile,
                PullEngine::ControlLatch controls);
        ~Session();
        void latch();
        PreparedEngine prepared;
        es_controls requested{sizeof(es_controls), ES_ABI_V1, 0, 0, 0, 0, 0};
        std::unique_ptr<DrivenShaft> drive;
        std::unique_ptr<PullEngine> pull;
    };
    es_telemetry snapshot(const Session &session) const;
    es_config m_config;
    SampleClock::Profile m_profile;
    es_telemetry m_snapshot{};
    std::unique_ptr<Session> m_session;
};
}
struct es_runtime {
    explicit es_runtime(es_config config) : implementation(config) {}
    engine_sim::Runtime implementation;
};
#endif
