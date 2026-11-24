#ifndef ENGINE_SIM_PRIVATE_FLEET_PCM_HPP
#define ENGINE_SIM_PRIVATE_FLEET_PCM_HPP

#include "engine_sim/fleet.h"

#include <array>
#include <atomic>
#include <memory>

namespace engine_sim::fleet {

inline constexpr std::uint32_t tileFrames = 64;

struct PcmTile {
    std::uint64_t epoch = 0, first = 0;
    std::array<float, tileFrames> mix{};
    std::array<std::array<float, tileFrames>, 8> stem{};
};

class PcmTransport {
public:
    static std::uint32_t highFrames(const es_rt_config &config) noexcept {
        return (config.profile == ES_RT_INTERACTIVE ? 3 : 4) * config.max_frames;
    }
    static std::uint32_t lowFrames(const es_rt_config &config) noexcept {
        return highFrames(config) - config.max_frames;
    }
    static std::size_t slotCount(const es_rt_config &config) noexcept {
        return highFrames(config) / tileFrames + 2;
    }

    explicit PcmTransport(std::size_t count)
        : m_count(count), m_slots(std::make_unique<PcmTile[]>(count)) {}

    bool lockFree() const noexcept {
        return m_read.is_lock_free() && m_write.is_lock_free() && m_broken.is_lock_free();
    }
    bool broken() const noexcept { return m_broken.load(std::memory_order_acquire) != 0; }

    // Producer only. The caller applies its timeline and generation-credit limits.
    bool tryPublish(const PcmTile &tile) noexcept;
    // Audio owner only. Records, frame range and destination selection are preflighted.
    std::uint32_t consume(std::uint64_t epoch, std::uint64_t first, std::uint32_t lanes,
                          const es_rt_audio &audio) noexcept;

private:
    friend struct PcmTransportTestAccess;

    const std::size_t m_count;
    const std::unique_ptr<PcmTile[]> m_slots;

    alignas(64) std::atomic<std::uint64_t> m_read{0};
    std::uint32_t m_offset = 0;
    alignas(64) std::atomic<std::uint64_t> m_write{0};
    alignas(64) std::atomic<std::uint32_t> m_broken{0};
};

}
#endif
