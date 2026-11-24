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
                // Until a measured row is installed, the owner is refused
                // before any roster, resource or partial startup can be reserved.
                if (!refused(config, ES_RT_UNSUPPORTED)) return 1;
                config.qualification_id = 1;
                if (!refused(config, ES_RT_UNSUPPORTED)) return 2;
                config.flags = ES_RT_EXPERIMENTAL;
                if (!refused(config, ES_RT_UNSUPPORTED)) return 3;
                config.qualification_id = 0;
                es_rt_fleet *experimental = nullptr;
                if (es_rt_create(&config, &experimental, nullptr) != ES_RT_OK || !experimental) return 4;
                es_rt_destroy(experimental);
                config.rate = 32000;
                if (!refused(config, ES_RT_UNSUPPORTED)) return 5;
                config.rate = rate;
                config.flags = ES_RT_EXPERIMENTAL | 2;
                if (!refused(config, ES_RT_UNSUPPORTED)) return 6;
                config.flags = ES_RT_EXPERIMENTAL;
                config.max_frames = 129;
                if (!refused(config, ES_RT_INVALID)) return 7;
            }
        }
    }
    std::puts("No qualified Fleet row is enabled; supported structural configurations use explicit experimental mode");
}
