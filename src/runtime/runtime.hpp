#ifndef ENGINE_SIM_PRIVATE_RUNTIME_HPP
#define ENGINE_SIM_PRIVATE_RUNTIME_HPP

#include "engine_sim/runtime.h"
#include "prepared_engine.h"
#include "pull_engine.h"
#include <memory>

namespace engine_sim {
struct RuntimeTestAccess;
class Runtime {
public:
    explicit Runtime(es_config config) noexcept;
    void load(const es_script::PreparationContext &recipe);
    void reset();
    void setControls(const es_controls &controls) noexcept;
    void render(float *output, std::uint32_t frames, std::uint32_t &written);
    void unload() noexcept;
    const es_telemetry &telemetry() const noexcept { return m_snapshot; }
private:
    friend struct RuntimeTestAccess;
    struct Session {
        Session(PreparedEngine value, std::uint32_t outputHz);
        void latch();
        PreparedEngine prepared;
        es_controls requested{sizeof(es_controls), ES_ABI_V1, 0, 0, 0, 0, 0};
        std::unique_ptr<PullEngine> pull;
    };
    es_telemetry snapshot(const Session &session) const;
    es_config m_config;
    es_telemetry m_snapshot{};
    std::unique_ptr<Session> m_session;
};
}
struct es_runtime {
    explicit es_runtime(es_config config) : implementation(config) {}
    engine_sim::Runtime implementation;
};
#endif
