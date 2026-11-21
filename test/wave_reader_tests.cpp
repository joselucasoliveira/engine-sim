#include <gtest/gtest.h>
#include "wave_reader.h"
#include "synthesizer.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>

namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
using engine_sim::WaveFailure;
void append(Bytes &bytes, std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<unsigned char>((value >> (8 * i)) & 255));
}
void store(Bytes &bytes, std::size_t offset, std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes[offset + i] = static_cast<unsigned char>((value >> (8 * i)) & 255);
}
Bytes format() {
    Bytes bytes;
    append(bytes, 1, 2); append(bytes, 1, 2); append(bytes, 44100, 4);
    append(bytes, 88200, 4); append(bytes, 2, 2); append(bytes, 16, 2);
    return bytes;
}
Bytes pcm(std::initializer_list<int16_t> values) {
    Bytes bytes;
    for (auto value : values) append(bytes, static_cast<uint16_t>(value), 2);
    return bytes;
}
using Chunk = std::pair<std::string, Bytes>;
Bytes wave(std::initializer_list<Chunk> chunks) {
    Bytes bytes{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    for (const auto &chunk : chunks) {
        bytes.insert(bytes.end(), chunk.first.begin(), chunk.first.end());
        append(bytes, static_cast<uint32_t>(chunk.second.size()), 4);
        bytes.insert(bytes.end(), chunk.second.begin(), chunk.second.end());
        if (chunk.second.size() % 2) bytes.push_back(0);
    }
    store(bytes, 4, static_cast<uint32_t>(bytes.size() - 8), 4);
    return bytes;
}
class WaveReader : public testing::Test {
protected:
    void SetUp() override {
        const auto *source = std::getenv("ENGINE_SIM_TEST_SOURCE");
        ASSERT_NE(source, nullptr);
        const auto relative = fs::canonical(fs::current_path()).lexically_relative(fs::canonical(source));
        ASSERT_TRUE(relative.empty() || *relative.begin() == "..");
    }
    fs::path write(const Bytes &bytes) {
        const auto path = fs::current_path() / (std::string(testing::UnitTest::GetInstance()->current_test_info()->name())
            + "-" + std::to_string(sequence++) + ".wav");
        std::ofstream stream(path, std::ios::binary);
        if (!bytes.empty()) stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream) throw std::runtime_error("cannot write external WAVE fixture");
        return path;
    }
    void reject(const Bytes &bytes, WaveFailure expected) {
        const auto path = write(bytes);
        try { engine_sim::readImpulseWave(path); ADD_FAILURE() << "accepted " << path; }
        catch (const engine_sim::WaveError &error) {
            EXPECT_EQ(error.failure, expected);
            EXPECT_EQ(error.path, path);
            EXPECT_FALSE(error.reason.empty());
            EXPECT_NE(std::string(error.what()).find(path.u8string()), std::string::npos);
        }
    }
    int sequence = 0;
};

TEST_F(WaveReader, SignedSamplesAndOddUnknownChunks) {
    const auto bytes = wave({{"JUNK", {1, 2, 3}}, {"fmt ", format()},
        {"LIST", {4}}, {"data", pcm({INT16_MIN, -1, 0, 1, INT16_MAX})}});
    EXPECT_EQ(engine_sim::readImpulseWave(write(bytes)), (std::vector<int16_t>{INT16_MIN, -1, 0, 1, INT16_MAX}));
}

TEST_F(WaveReader, DataBeforeFormatAndExtendedPcm) {
    auto extended = format();
    append(extended, 0, 2);
    const auto bytes = wave({{"data", pcm({-1, INT16_MAX})}, {"fmt ", extended}});
    EXPECT_EQ(engine_sim::readImpulseWave(write(bytes)), (std::vector<int16_t>{-1, INT16_MAX}));
}

TEST_F(WaveReader, SilenceAndOneTapKeepConvolutionGain) {
    for (const int16_t value : {int16_t{0}, int16_t{INT16_MAX}}) {
        const auto samples = engine_sim::readImpulseWave(write(wave({{"fmt ", format()}, {"data", pcm({value})}})));
        ASSERT_EQ(samples, (std::vector<int16_t>{value}));
        Synthesizer synth;
        synth.initialize(Synthesizer::Parameters{});
        synth.initializeImpulseResponse(samples.data(), static_cast<unsigned>(samples.size()), 0.25f, 0);
        EXPECT_FLOAT_EQ(synth.m_filters[0].convolution.f(1), value == 0 ? 0 : 0.25f);
        EXPECT_FLOAT_EQ(synth.m_filters[0].convolution.f(0), 0);
    }
}

TEST_F(WaveReader, RejectsTruncationAtEveryBoundary) {
    const auto complete = wave({{"fmt ", format()}, {"JUNK", {1, 2, 3}}, {"data", pcm({-1})}});
    for (std::size_t size = 0; size < complete.size(); ++size) {
        SCOPED_TRACE(size);
        Bytes truncated(complete.begin(), complete.begin() + size);
        if (size >= 8) store(truncated, 4, static_cast<uint32_t>(size - 8), 4);
        EXPECT_THROW(engine_sim::readImpulseWave(write(truncated)), engine_sim::WaveError);
    }
}

TEST_F(WaveReader, RejectsContainersLengthsAndMissingPadding) {
    const auto complete = wave({{"fmt ", format()}, {"data", pcm({1})}});
    auto invalid = complete; invalid[0] = 'X'; reject(invalid, WaveFailure::Container);
    invalid = complete; store(invalid, 4, UINT32_MAX, 4); reject(invalid, WaveFailure::Container);
    invalid = complete; invalid.push_back(0); reject(invalid, WaveFailure::Container);
    invalid = complete; store(invalid, 16, UINT32_MAX, 4); reject(invalid, WaveFailure::Chunk);
    invalid = wave({{"fmt ", format()}, {"data", pcm({1})}, {"JUNK", {1}}});
    invalid.pop_back(); store(invalid, 4, static_cast<uint32_t>(invalid.size() - 8), 4);
    reject(invalid, WaveFailure::Chunk);
    invalid = complete; invalid.push_back(0); store(invalid, 4, static_cast<uint32_t>(invalid.size() - 8), 4);
    reject(invalid, WaveFailure::Chunk);
}

TEST_F(WaveReader, RejectsUnsupportedOrInconsistentFormats) {
    struct Mutation { unsigned offset, value, count; };
    for (const auto mutation : {Mutation{0, 3, 2}, {2, 2, 2}, {4, 48000, 4}, {8, 123, 4}, {12, 4, 2}, {14, 24, 2}}) {
        SCOPED_TRACE(mutation.offset);
        auto invalid = format(); store(invalid, mutation.offset, mutation.value, mutation.count);
        reject(wave({{"fmt ", invalid}, {"data", pcm({1})}}), WaveFailure::Format);
    }
    for (unsigned length = 0; length < 16; ++length) {
        auto invalid = format(); invalid.resize(length);
        reject(wave({{"fmt ", invalid}, {"data", pcm({1})}}), WaveFailure::Format);
    }
    auto extension = format(); extension.push_back(0);
    reject(wave({{"fmt ", extension}, {"data", pcm({1})}}), WaveFailure::Format);
    extension = format(); append(extension, UINT16_MAX, 2);
    reject(wave({{"fmt ", extension}, {"data", pcm({1})}}), WaveFailure::Format);
}

TEST_F(WaveReader, RejectsMissingDuplicateOrEmptyChunks) {
    reject(wave({}), WaveFailure::Data);
    reject(wave({{"fmt ", format()}}), WaveFailure::Data);
    reject(wave({{"data", pcm({1})}}), WaveFailure::Data);
    reject(wave({{"fmt ", format()}, {"fmt ", format()}, {"data", pcm({1})}}), WaveFailure::Format);
    reject(wave({{"fmt ", format()}, {"data", pcm({1})}, {"data", pcm({1})}}), WaveFailure::Data);
    reject(wave({{"fmt ", format()}, {"data", {}}}), WaveFailure::Data);
    reject(wave({{"fmt ", format()}, {"data", {1}}}), WaveFailure::Data);
}

TEST_F(WaveReader, ReportsIoAndSizeWithPath) {
    const auto missing = fs::current_path() / "missing-impulse.wav";
    try { engine_sim::readImpulseWave(missing); FAIL() << "accepted missing file"; }
    catch (const engine_sim::WaveError &error) {
        EXPECT_EQ(error.failure, WaveFailure::Io); EXPECT_EQ(error.path, missing);
    }
    reject({}, WaveFailure::Size);
    const auto path = write({});
    { std::ofstream stream(path, std::ios::binary); stream.seekp(engine_sim::maxImpulseWaveBytes); stream.put(0); ASSERT_TRUE(stream); }
    ASSERT_EQ(fs::file_size(path), engine_sim::maxImpulseWaveBytes + 1);
    try { engine_sim::readImpulseWave(path); FAIL() << "accepted oversized file"; }
    catch (const engine_sim::WaveError &error) { EXPECT_EQ(error.failure, WaveFailure::Size); EXPECT_EQ(error.path, path); }
}

void littleEndian(std::ostream &stream, uint32_t bits, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) stream.put(static_cast<char>((bits >> (8 * i)) & 255));
}
}

int main(int argc, char **argv) {
    if (argc == 5 && std::string_view(argv[1]) == "--decode") {
        try {
            const auto samples = engine_sim::readImpulseWave(fs::u8path(argv[2]));
            std::ofstream raw(fs::u8path(std::string(argv[3]) + ".s16le"), std::ios::binary);
            std::ofstream response(fs::u8path(std::string(argv[3]) + ".f32le"), std::ios::binary);
            raw.exceptions(std::ios::failbit | std::ios::badbit);
            response.exceptions(std::ios::failbit | std::ios::badbit);
            for (auto sample : samples) littleEndian(raw, static_cast<uint16_t>(sample), 2);
            Synthesizer synth;
            synth.initialize(Synthesizer::Parameters{});
            synth.initializeImpulseResponse(samples.data(), static_cast<unsigned>(samples.size()), std::stof(argv[4]), 0);
            for (int i = 0; i < 10008; ++i) {
                const auto value = synth.m_filters[0].convolution.f(i == 0 ? 1.0f : 0.0f);
                uint32_t bits; std::memcpy(&bits, &value, 4); littleEndian(response, bits, 4);
            }
            std::cout << samples.size() << '\n';
            return 0;
        }
        catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 2; }
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
