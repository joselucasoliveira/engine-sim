#include "../include/wave_reader.h"

#include <algorithm>
#include <fstream>
#include <string_view>

namespace engine_sim {
std::vector<std::int16_t> readImpulseWave(const std::filesystem::path &path) {
    const auto fail = [&](WaveFailure failure, const char *reason) { throw WaveError(failure, path, reason); };
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) fail(WaveFailure::Io, "cannot inspect impulse file");
    if (size < 12 || size > maxImpulseWaveBytes) fail(WaveFailure::Size, "WAVE size must be between 12 bytes and 64 MiB");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    if (!stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        fail(WaveFailure::Io, "cannot read complete impulse file");
    }
    const auto tag = [&](std::size_t offset, std::string_view name) {
        return std::equal(name.begin(), name.end(), bytes.begin() + offset);
    };
    const auto le = [&](std::size_t offset, unsigned count) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i) value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8 * i);
        return value;
    };
    if (!tag(0, "RIFF") || !tag(8, "WAVE")) fail(WaveFailure::Container, "expected little-endian RIFF/WAVE");
    if (le(4, 4) != bytes.size() - 8) fail(WaveFailure::Container, "RIFF length differs from file size");
    bool haveFormat = false, haveData = false;
    std::size_t dataOffset = 0, dataLength = 0;
    for (std::size_t offset = 12; offset < bytes.size();) {
        if (bytes.size() - offset < 8) fail(WaveFailure::Chunk, "truncated chunk header");
        const std::size_t length = le(offset + 4, 4), payload = offset + 8;
        if (length > bytes.size() - payload) fail(WaveFailure::Chunk, "chunk payload exceeds file size");
        const auto padding = length % 2;
        if (padding > bytes.size() - payload - length) fail(WaveFailure::Chunk, "missing odd chunk padding");
        if (tag(offset, "fmt ")) {
            if (haveFormat || length < 16) fail(WaveFailure::Format, "missing or duplicate PCM format fields");
            if (length == 17 || (length >= 18 && le(payload + 16, 2) > length - 18)) {
                fail(WaveFailure::Format, "format extension exceeds chunk size");
            }
            if (le(payload, 2) != 1 || le(payload + 2, 2) != 1 || le(payload + 4, 4) != 44100
                || le(payload + 14, 2) != 16) {
                fail(WaveFailure::Format, "expected PCM1 mono signed 16-bit samples at 44100 Hz");
            }
            if (le(payload + 8, 4) != 88200 || le(payload + 12, 2) != 2) {
                fail(WaveFailure::Format, "inconsistent byte rate or block alignment");
            }
            haveFormat = true;
        }
        else if (tag(offset, "data")) {
            if (haveData || length == 0 || length % 2 != 0) fail(WaveFailure::Data, "expected one nonempty whole-frame data chunk");
            dataOffset = payload; dataLength = length; haveData = true;
        }
        offset = payload + length + padding;
    }
    if (!haveFormat || !haveData) fail(WaveFailure::Data, "missing format or sample data");
    std::vector<std::int16_t> samples;
    samples.reserve(dataLength / 2);
    for (std::size_t i = dataOffset; i < dataOffset + dataLength; i += 2) {
        const int value = static_cast<int>(le(i, 2));
        samples.push_back(static_cast<std::int16_t>(value < 32768 ? value : value - 65536));
    }
    return samples;
}
}
