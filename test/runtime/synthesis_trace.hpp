#ifndef ENGINE_SIM_SYNTHESIS_TEST_TRACE_HPP
#define ENGINE_SIM_SYNTHESIS_TEST_TRACE_HPP

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace synthesis_test {
template<class UInt, class Float>
UInt bits(Float value) {
    UInt word;
    static_assert(sizeof(UInt) == sizeof(Float));
    std::memcpy(&word, &value, sizeof word);
    return word;
}

inline void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

struct Trace {
    void write(std::uint64_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    }

    void save(const std::filesystem::path &file) const {
        require(!std::filesystem::exists(file), "Synthesis evidence already exists");
        std::ofstream output(file, std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        output.close();
        require(bool(output), "Cannot save synthesis evidence");
    }

    std::vector<std::uint8_t> bytes;
};
}

#endif
