#include <engine_sim/fleet.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "32-bit lock-free atomic required");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "64-bit lock-free atomic required");
static_assert(sizeof(void *) == 8, "64-bit native consumer required");

int main() {
    const es_rt_config config{sizeof(config), ES_RT_V1, 48000, ES_RT_INTERACTIVE, 128, 1, 0, 0, 317};
    es_rt_fleet *fleet = nullptr;
    if (es_rt_create(&config, &fleet, nullptr) != ES_RT_OK) return 1;
    if (es_rt_start(fleet, nullptr) != ES_RT_OK) {
        es_rt_destroy(fleet);
        return 2;
    }
    bool passed = false;
    std::thread callback([&] {
        float sample = 77.0f;
        const es_rt_audio audio{sizeof(audio), ES_RT_V1, 1, 1, &sample, {}};
        es_rt_block result{sizeof(result), ES_RT_V1, 0, 0, 0, 0};
        passed = es_rt_process(fleet, &audio, &result) == ES_RT_OK &&
            result.first == 0 && result.epoch == 317 && result.pcm_frames == 1 &&
            result.fallback_frames == 0 && sample == 0.0f;
    });
    callback.join();
    const auto stopped = es_rt_quiesce(fleet);
    es_rt_destroy(fleet);
    if (!passed || stopped != ES_RT_OK) return 3;
    std::puts("fresh callback returned the first frame; native 32-bit and 64-bit atomics are lock-free");
}
