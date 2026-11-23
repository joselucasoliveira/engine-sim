#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace engine_sim::legacy::cli {
namespace {

constexpr std::array<std::uint32_t, 64> constants{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

constexpr std::uint32_t rotate(std::uint32_t bits, unsigned count) noexcept {
    return (bits >> count) | (bits << (32 - count));
}

class Hash {
public:
    void add(std::string_view bytes) {
        m_size += bytes.size();
        while (!bytes.empty()) {
            const auto amount = std::min(bytes.size(), m_block.size() - m_used);
            std::memcpy(m_block.data() + m_used, bytes.data(), amount);
            m_used += amount;
            bytes.remove_prefix(amount);
            if (m_used == m_block.size()) {
                compress();
                m_used = 0;
            }
        }
    }

    std::string finish() {
        const std::uint64_t bits = 8 * m_size;
        m_block[m_used++] = 0x80u;
        if (m_used > 56) {
            std::fill(m_block.begin() + static_cast<std::ptrdiff_t>(m_used), m_block.end(), 0);
            compress();
            m_used = 0;
        }
        std::fill(m_block.begin() + static_cast<std::ptrdiff_t>(m_used), m_block.begin() + 56, 0);
        for (unsigned i = 0; i < 8; ++i) m_block[63 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
        compress();

        std::string hex;
        for (auto word : m_words)
            for (int bits_left = 28; bits_left >= 0; bits_left -= 4)
                hex += "0123456789abcdef"[(word >> bits_left) & 0xfu];
        return hex;
    }

private:
    void compress() {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto start = 4 * i;
            for (std::size_t j = 0; j < 4; ++j) w[i] = (w[i] << 8) | m_block[start + j];
        }
        for (std::size_t i = 16; i < w.size(); ++i) {
            const auto a = w[i - 15], b = w[i - 2];
            const auto s0 = rotate(a, 7) ^ rotate(a, 18) ^ (a >> 3);
            const auto s1 = rotate(b, 17) ^ rotate(b, 19) ^ (b >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        auto [a, b, c, d, e, f, g, h] = m_words;
        for (std::size_t i = 0; i < w.size(); ++i) {
            const auto left = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
            const auto right = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto carry = h + right + choose + constants[i] + w[i];
            h = g;
            g = f;
            f = e;
            e = d + carry;
            d = c;
            c = b;
            b = a;
            a = carry + left + majority;
        }
        const std::array<std::uint32_t, 8> words{a, b, c, d, e, f, g, h};
        for (std::size_t i = 0; i < words.size(); ++i) m_words[i] += words[i];
    }

    std::array<std::uint32_t, 8> m_words{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::array<std::uint8_t, 64> m_block{};
    std::size_t m_used = 0;
    std::uint64_t m_size = 0;
};
}

std::string sha256(std::string_view bytes) {
    Hash hash;
    hash.add(bytes);
    return hash.finish();
}

std::string sha256_file(const std::filesystem::path &file) {
    Hash hash;
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot hash " + file.u8string());
    std::array<char, 64 * 1024> block{};
    while (input) {
        input.read(block.data(), static_cast<std::streamsize>(block.size()));
        hash.add({block.data(), static_cast<std::size_t>(input.gcount())});
    }
    if (!input.eof() || input.bad()) throw std::runtime_error("Cannot finish hashing " + file.u8string());
    return hash.finish();
}

}
