#ifndef ENGINE_SIM_DESKTOP_ENGINE_VIEWS_H
#define ENGINE_SIM_DESKTOP_ENGINE_VIEWS_H

#include "simulator.h"
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <typeindex>
#include <unordered_map>

// Private host bridge, deliberately absent from the installed C interface.
namespace desktop {
class EngineViews;
struct ViewEpoch {
    std::thread::id owner = std::this_thread::get_id();
    bool alive = true;
    unsigned readers = 0;
    std::uint64_t generation;
    std::unordered_map<std::type_index, std::vector<const void *>> topology;
    void check() const {
        if (owner != std::this_thread::get_id() || !alive || readers == 0)
            throw std::logic_error("Engine observation is stale or outside its owner presentation phase");
    }
};

// Only a topology index and a weak generation token survive a presentation call.
template<class T> class ViewHandle {
public:
    ViewHandle() = default;
    ViewHandle(std::nullptr_t) {}
    const T *operator->() const {
        auto epoch = m_epoch.lock();
        if (!epoch) throw std::logic_error("Retired engine observation");
        epoch->check();
        return static_cast<const T *>(epoch->topology.at(typeid(T)).at(m_index));
    }
    bool valid() const {
        auto epoch = m_epoch.lock();
        return epoch && epoch->alive;
    }
    bool operator==(std::nullptr_t) const { return !valid(); }
    bool operator!=(std::nullptr_t) const { return valid(); }
    std::uint64_t generation() const { return m_generation; }
private:
    friend class EngineViews;
    std::weak_ptr<ViewEpoch> m_epoch;
    std::size_t m_index = 0;
    std::uint64_t m_generation = 0;
};

class PresentationLease {
public:
    explicit PresentationLease(std::shared_ptr<ViewEpoch> epoch) : m_epoch(std::move(epoch)) {
        if (m_epoch->owner != std::this_thread::get_id() || !m_epoch->alive)
            throw std::logic_error("Invalid engine presentation owner");
        ++m_epoch->readers;
    }
    ~PresentationLease() { --m_epoch->readers; }
    PresentationLease(const PresentationLease &) = delete;
    PresentationLease &operator=(const PresentationLease &) = delete;
private:
    std::shared_ptr<ViewEpoch> m_epoch;
};

struct ScopeObservation {
    std::uint64_t tick;
    double cycle, pressureCycle, exhaust, pressure, cylinderExhaust, intake;
    double molecules, exhaustLift, intakeLift, volume, staticPressure, rpm;
};

class EngineViews {
public:
    static constexpr std::size_t ScopeCapacity = 8192;
    explicit EngineViews(const Simulator &simulator);
    ~EngineViews() { invalidate(); }
    EngineViews(const EngineViews &) = delete;
    EngineViews &operator=(const EngineViews &) = delete;
    PresentationLease present() const { return PresentationLease(m_epoch); }
    void invalidate() noexcept { m_epoch->alive = false; }
    void requireQuiescent() const;
    template<class T> ViewHandle<T> at(std::size_t index = 0) const {
        // Bounds checked during detached staging, before publishing any handles.
        (void)m_epoch->topology.at(typeid(T)).at(index);
        ViewHandle<T> result;
        result.m_epoch = m_epoch; result.m_index = index; result.m_generation = m_epoch->generation;
        return result;
    }
    void sampleTick(const Simulator &simulator);
    bool popScope(ScopeObservation &point);
    void collectIgnition(Simulator &simulator);
    bool fired(int index) const { return m_fired.at(index); }
    std::uint64_t evictedScopePoints() const { return m_evicted; }
    std::uint64_t observedTicks() const { return m_ticks; }
private:
    template<class T> void add(const T *pointer) { m_epoch->topology[typeid(T)].push_back(pointer); }
    std::shared_ptr<ViewEpoch> m_epoch;
    std::vector<bool> m_fired;
    std::array<ScopeObservation, ScopeCapacity> m_scopes{};
    std::size_t m_head = 0, m_size = 0;
    std::uint64_t m_evicted = 0, m_ticks = 0;
};

// Purposeful host commands. Input collection edits pending values; flush happens
// after the input read lease ends. Physics tick latches retain the old contract.
class EngineCommands {
public:
    struct Values {
        double demand = 0, clutch = 0, dynoSpeed = 0;
        bool ignition = false, starter = false, dyno = false, hold = false;
        int frequency = 10000;
    } pending, applied;
    explicit EngineCommands(Simulator &simulator, EngineViews &views);
    void setDemand(double value) { pending.demand = value; }
    void setClutch(double value) { pending.clutch = value; }
    void setIgnition(bool value) { pending.ignition = value; }
    void setStarter(bool value) { pending.starter = value; }
    void setDyno(bool enabled, bool hold, double speed) {
        pending.dyno = enabled; pending.hold = hold; pending.dynoSpeed = speed;
    }
    void setFrequency(int value) { pending.frequency = value; }
    void changeGear(int value) {
        if (value >= -1 && value < m_simulator.getTransmission()->getGearCount()) m_gear = value;
    }
    int gear() const { return m_gear; }
    void setMixer(const Synthesizer::AudioParameters &value) { m_mixer = value; m_mixerDirty = true; }
    const Synthesizer::AudioParameters &mixer() const { return m_mixer; }
    void flush();
    void setPresentationSpeed(double speed) { m_views.requireQuiescent(); m_simulator.setSimulationSpeed(speed); }
    void latch() { applied = pending; }
    void apply(const Values &values, bool frequency);
private:
    Simulator &m_simulator;
    EngineViews &m_views;
    int m_gear;
    bool m_mixerDirty = false;
    Synthesizer::AudioParameters m_mixer;
};
}
#endif
