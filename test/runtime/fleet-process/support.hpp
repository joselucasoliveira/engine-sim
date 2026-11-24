#ifndef ENGINE_SIM_TEST_FLEET_PROCESS_SUPPORT_HPP
#define ENGINE_SIM_TEST_FLEET_PROCESS_SUPPORT_HPP

#include "owner.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace fleet_process_test {
inline constexpr std::uint64_t epoch = 19;
inline constexpr float sentinel = -426;
inline constexpr std::uint32_t capacity = 1024;
using engine_sim::fleet::PcmTile;
using Owner = std::unique_ptr<es_rt_fleet, decltype(&es_rt_destroy)>;

inline void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

inline Owner create(std::uint32_t maximum, std::uint32_t lanes,
                    std::uint32_t profile = ES_RT_BALANCED) {
    es_rt_config config{sizeof(config), ES_RT_V1, 48000, profile, maximum, lanes, ES_RT_EXPERIMENTAL, 0, epoch};
    es_rt_fleet *fleet = nullptr;
    require(es_rt_create(&config, &fleet, nullptr) == ES_RT_OK && fleet, "create transport");
    return {fleet, es_rt_destroy};
}

inline float sample(std::uint32_t plane, std::uint64_t frame) {
    return frame % 127 == 0 ? -0.0f : static_cast<float>(1 + (frame % 100 * 9 + plane));
}

inline PcmTile tile(std::uint64_t first) {
    PcmTile result;
    result.epoch = epoch;
    result.first = first;
    for (std::uint32_t i = 0; i < result.mix.size(); ++i) {
        result.mix[i] = sample(0, first + i);
        for (std::uint32_t s = 0; s < result.stem.size(); ++s) result.stem[s][i] = sample(s + 1, first + i);
    }
    return result;
}

struct Call {
    std::array<std::array<float, capacity + 2>, 9> output;
    es_rt_audio audio{sizeof(audio), ES_RT_V1, 0, capacity, nullptr, {}};
    es_rt_block block{sizeof(block), ES_RT_V1, 5, 7, 11, 13};

    Call(std::uint32_t frames, unsigned mask) {
        for (auto &plane : output) plane.fill(sentinel);
        audio.frames = frames;
        if (mask & 1) audio.mix = output[0].data() + 1;
        for (std::uint32_t s = 0; s < 8; ++s) {
            if (mask & (2u << s)) audio.stem[s] = output[s + 1].data() + 1;
        }
    }

    void check(std::int32_t status, std::uint64_t first, std::uint32_t delivered) const {
        require(status == (delivered == audio.frames ? ES_RT_OK : ES_RT_BROKEN), "process status");
        require(block.size == sizeof(block) && block.abi == ES_RT_V1 && block.epoch == epoch &&
                block.first == first && block.pcm_frames == delivered &&
                block.fallback_frames == audio.frames - delivered, "block result fields");
        for (std::uint32_t p = 0; p < output.size(); ++p) {
            const bool selected = p ? audio.stem[p - 1] != nullptr : audio.mix != nullptr;
            for (std::uint32_t i = 0; i < output[p].size(); ++i) {
                float expected = sentinel;
                if (selected && i > 0 && i <= audio.frames)
                    expected = i <= delivered ? sample(p, first + i - 1) : 0.0f;
                require(std::memcmp(&expected, &output[p][i], sizeof(expected)) == 0,
                        "PCM, fallback bit pattern or plane canary");
            }
        }
    }

    void rejected(es_rt_fleet *fleet, std::int32_t expected, const es_rt_audio *input, es_rt_block *result) {
        const auto original = block;
        const auto first = fleet ? fleet->commands.submitted.load() : 0;
        require(es_rt_process(fleet, input, result) == expected, "invalid record status");
        require(std::memcmp(&original, &block, sizeof(block)) == 0, "invalid record changed result");
        require(!fleet || fleet->commands.submitted.load() == first, "invalid record advanced D");
        for (const auto &plane : output) {
            for (float value : plane) require(value == sentinel, "invalid record modified samples");
        }
    }
};
}
#endif
