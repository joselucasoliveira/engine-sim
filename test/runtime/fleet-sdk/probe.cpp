#include <engine_sim/fleet.h>
#include "observer.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <thread>

extern "C" std::int32_t fleet_probe_boundary(es_rt_fleet *, const es_rt_audio *, es_rt_block *,
                                             fleet_probe_control, void *);

int main() {
    auto setup = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "fleet_probe_setup"));
    auto arm = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "fleet_probe_arm"));
    auto stop = reinterpret_cast<void (*)(fleet_probe_hits *)>(dlsym(RTLD_DEFAULT, "fleet_probe_end"));
    if (!setup || !arm || !stop || !setup()) return 1;
    es_rt_config config{sizeof(config), ES_RT_V1, 44100, ES_RT_BALANCED, 64, 1, ES_RT_EXPERIMENTAL, 0, 17};
    es_rt_fleet *fleet = nullptr;
    if (es_rt_create(&config, &fleet, nullptr) != ES_RT_OK) return 2;
    if (es_rt_start(fleet, nullptr) != ES_RT_OK) {
        es_rt_destroy(fleet);
        return 2;
    }
    std::array<fleet_probe_hits, PROBE_CONTROL_COUNT> observed{};
    bool correctFrames = false;
    std::thread callback([&] {
        float sample = 7.0f;
        const es_rt_audio audio{sizeof(audio), ES_RT_V1, 1, 1, &sample, {}};
        es_rt_block reply{sizeof(reply), ES_RT_V1, 0, 0, 0, 0};
        auto *release = std::malloc(64);
        if (!release) return;
        bool valid = true;
        for (unsigned index = 0; index < observed.size(); ++index) {
            const auto control = static_cast<fleet_probe_control>(index);
            arm();
            const auto status = fleet_probe_boundary(fleet, &audio, &reply, control, release);
            stop(&observed[index]);
            valid &= status == ES_RT_OK && reply.first == index && reply.pcm_frames == 1 && sample == 0.0f;
        }
        correctFrames = valid;
    });
    callback.join();
    const auto stopped = es_rt_quiesce(fleet);
    es_rt_destroy(fleet);
    const std::array<fleet_probe_hits, PROBE_CONTROL_COUNT> expected{{
        {{0, 0, 0, 0, 0, 0}}, {{1, 1, 0, 0, 0, 0}}, {{0, 1, 0, 0, 0, 0}},
        {{0, 1, 1, 0, 0, 0}}, {{0, 0, 0, 1, 1, 0}}, {{0, 0, 0, 0, 0, 1}},
    }};
    bool correctControls = true;
    for (unsigned control = 0; control < observed.size(); ++control) {
        for (unsigned op = 0; op < PROBE_OPERATION_COUNT; ++op) {
            const auto count = observed[control].count[op];
            std::printf("control=%u operation=%u count=%u\n", control, op, count);
            // The native exception runtime may allocate and release exception storage.
            if (control == PROBE_THROW && (op == PROBE_MALLOC || op == PROBE_FREE)) continue;
            correctControls &= count == expected[control].count[op];
        }
    }
    if (!correctFrames || stopped != ES_RT_OK || !correctControls) return 3;
    std::puts("installed shared GNU observer cold=0 injected=2 release=1 aligned=1 lock=1 wait=1 throw=1");
}
