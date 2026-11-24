#include <engine_sim/fleet.h>
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <thread>

extern "C" std::int32_t fleet_probe_boundary(es_rt_fleet *, const es_rt_audio *, es_rt_block *, int);

int main() {
    auto arm = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "fleet_probe_arm"));
    auto stop = reinterpret_cast<unsigned (*)()>(dlsym(RTLD_DEFAULT, "fleet_probe_end"));
    if (!arm || !stop) return 1;
    es_rt_config config{sizeof(config), ES_RT_V1, 44100, ES_RT_BALANCED, 64, 1, ES_RT_EXPERIMENTAL, 0, 17};
    es_rt_fleet *fleet = nullptr;
    if (es_rt_create(&config, &fleet, nullptr) != ES_RT_OK || es_rt_start(fleet, nullptr) != ES_RT_OK) return 2;
    unsigned ordinary = 1, injected = 0;
    int codes = 0;
    std::thread callback([&] {
        float sample = 7;
        const es_rt_audio audio{sizeof(audio), ES_RT_V1, 1, 1, &sample, {}};
        es_rt_block result{sizeof(result), ES_RT_V1, 0, 0, 0, 0};
        arm();
        codes |= fleet_probe_boundary(fleet, &audio, &result, 0);
        ordinary = stop();
        arm();
        codes |= fleet_probe_boundary(fleet, &audio, &result, 1);
        injected = stop();
        if (sample != 0 || result.first != 1 || result.pcm_frames != 1) codes = 1;
    });
    callback.join();
    codes |= es_rt_quiesce(fleet);
    es_rt_destroy(fleet);
    std::printf("installed shared GNU heap observer cold=%u injected=%u\n", ordinary, injected);
    return codes || ordinary != 0 || injected != 2;
}
