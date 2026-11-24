#ifndef ENGINE_SIM_PRIVATE_FLEET_SNAPSHOT_HPP
#define ENGINE_SIM_PRIVATE_FLEET_SNAPSHOT_HPP

#include "engine_sim/fleet.h"

#include <array>
#include <atomic>

namespace engine_sim::fleet {

class Snapshots {
public:
    template<class Fill>
    void publish(Fill fill) noexcept {
        const auto write = m_write.load(std::memory_order_relaxed);
        const auto read = m_read.load(std::memory_order_acquire);
        const auto dropped = m_dropped.load(std::memory_order_relaxed);
        if (write == UINT64_MAX || write - read == m_records.size()) {
            if (dropped != UINT64_MAX) m_dropped.store(dropped + 1, std::memory_order_relaxed);
            return;
        }
        auto &next = m_records[write % m_records.size()];
        fill(next);
        next.dropped = dropped;
        m_write.store(write + 1, std::memory_order_release);
    }

    bool poll(es_rt_snapshot &out) noexcept {
        const auto read = m_read.load(std::memory_order_relaxed);
        if (read == m_write.load(std::memory_order_acquire)) return false;
        const auto size = out.size, abi = out.abi;
        out = m_records[read % m_records.size()];
        out.size = size;
        out.abi = abi;
        m_read.store(read + 1, std::memory_order_release);
        return true;
    }

    std::uint64_t dropped() const noexcept { return m_dropped.load(std::memory_order_relaxed); }
    bool lockFree() const noexcept {
        return m_read.is_lock_free() && m_write.is_lock_free() && m_dropped.is_lock_free();
    }

private:
    alignas(64) std::atomic<std::uint64_t> m_read{0};
    alignas(64) std::atomic<std::uint64_t> m_write{0};
    std::atomic<std::uint64_t> m_dropped{0};
    std::array<es_rt_snapshot, 16> m_records{};
};

}
#endif
