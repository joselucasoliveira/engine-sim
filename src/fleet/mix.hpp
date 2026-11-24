#ifndef ENGINE_SIM_PRIVATE_FLEET_MIX_HPP
#define ENGINE_SIM_PRIVATE_FLEET_MIX_HPP

#include "pcm.hpp"

#include <algorithm>

namespace engine_sim::fleet {

inline void writeMix(PcmTile &tile, std::uint32_t lanes,
                     const std::array<double, tileFrames> &master) noexcept {
    for (std::uint32_t frame = 0; frame < tileFrames; ++frame) {
        double sum = 0;
        for (std::uint32_t lane = 0; lane < lanes; ++lane) sum += static_cast<double>(tile.stem[lane][frame]);
        tile.mix[frame] = static_cast<float>(std::clamp(sum * master[frame], -1., 1.));
    }
}

}
#endif
