#ifndef ENGINE_SIM_WAVE_READER_H
#define ENGINE_SIM_WAVE_READER_H

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine_sim {
enum class WaveFailure { Io, Size, Container, Chunk, Format, Data };
class WaveError : public std::runtime_error {
public:
    WaveError(WaveFailure failure, const std::filesystem::path &path, const std::string &reason)
        : std::runtime_error(path.u8string() + ": " + reason), failure(failure), path(path), reason(reason) {}
    WaveFailure failure;
    std::filesystem::path path;
    std::string reason;
};

// The synthesis input contract is mono signed PCM16 LE at 44100 Hz, without resampling.
inline constexpr std::uintmax_t maxImpulseWaveBytes = 64 * 1024 * 1024;
std::vector<std::int16_t> decodeImpulseWave(
    const std::vector<unsigned char> &bytes, const std::filesystem::path &source);
std::vector<unsigned char> readImpulseWaveBytes(const std::filesystem::path &path);
std::vector<std::int16_t> readImpulseWave(const std::filesystem::path &path);
}
#endif
