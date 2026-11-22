#ifndef ENGINE_SIM_RANDOM_STREAM_H
#define ENGINE_SIM_RANDOM_STREAM_H

#include <cstdint>

namespace engine_sim {
class RandomStream {
public:
    RandomStream(std::uint64_t seed = 1, std::uint32_t domain = 1, std::uint32_t index = 0) {
        reset(seed, domain, index);
    }

    static std::uint64_t mix(std::uint64_t word) {
        word = (word ^ (word >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        word = (word ^ (word >> 27)) * UINT64_C(0x94d049bb133111eb);
        return word ^ (word >> 31);
    }

    void reset(std::uint64_t seed, std::uint32_t domain, std::uint32_t index) {
        m_state = mix(seed ^ ((std::uint64_t{domain} << 32) | index));
    }

    std::uint64_t next() {
        m_state += UINT64_C(0x9e3779b97f4a7c15);
        return mix(m_state);
    }

    double uniform() {
        return static_cast<double>(next() >> 11) * 0x1.0p-53;
    }

private:
    std::uint64_t m_state;
};
}
#endif
