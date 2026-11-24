#include <engine_sim/fleet.h>

#include <array>
#include <cstdint>
#include <cstdio>

namespace {
bool refused(const es_rt_config &config, std::int32_t status) {
    es_rt_fleet *fleet = nullptr;
    es_rt_diagnostic diagnostics{sizeof(diagnostics), ES_RT_V1, 0, 0, {}};
    const auto actual = es_rt_create(&config, &fleet, &diagnostics);
    const bool neverCreated = fleet == nullptr;
    es_rt_destroy(fleet);
    return actual == status && neverCreated && diagnostics.message[0] != '\0';
}

bool accepts(const es_rt_config &config) {
    es_rt_fleet *fleet = nullptr;
    const auto status = es_rt_create(&config, &fleet, nullptr);
    const bool created = status == ES_RT_OK && fleet != nullptr;
    es_rt_destroy(fleet);
    return created;
}
}

int main() {
    constexpr std::array<std::uint32_t, 5> spans{64, 128, 256, 512, 1024};
    constexpr std::array<std::uint32_t, 2> rates{44100, 48000};
    for (const auto rate : rates) {
        for (const auto profile : {ES_RT_INTERACTIVE, ES_RT_BALANCED}) {
            for (const auto span : spans) {
                if (profile == ES_RT_INTERACTIVE && span > 128) continue;
                es_rt_config config{sizeof(config), ES_RT_V1, rate, static_cast<std::uint32_t>(profile),
                                    span, 2, 0, 0, 403};
                if (!accepts(config)) return 1;
                config.qualification_id = 1;
                if (!refused(config, ES_RT_UNSUPPORTED)) return 2;
                config.qualification_id = 0;
                config.flags = 1;
                if (!refused(config, ES_RT_UNSUPPORTED)) return 3;
                config.flags = 2;
                if (!refused(config, ES_RT_UNSUPPORTED)) return 4;
                config.flags = 0;
                config.rate = 32000;
                if (!refused(config, ES_RT_UNSUPPORTED)) return 5;
                config.rate = rate;
                config.max_frames = 129;
                if (!refused(config, ES_RT_INVALID)) return 6;
            }
        }
    }
    std::puts("Fleet profiles accept default creation. Flags and qualification IDs are reserved.");
}
