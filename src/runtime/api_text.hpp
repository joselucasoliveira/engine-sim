#ifndef ENGINE_SIM_PRIVATE_API_TEXT_HPP
#define ENGINE_SIM_PRIVATE_API_TEXT_HPP

#include "engine_sim/runtime.h"

#include <cstddef>
#include <cstring>
#include <string_view>

namespace engine_sim::api {
inline std::size_t character(std::string_view text) noexcept {
    const auto *bytes = reinterpret_cast<const unsigned char *>(text.data());
    const auto first = bytes[0];
    if (first < 0x80) return first ? 1 : 0;
    const unsigned length = first >= 0xc2 && first <= 0xdf ? 2 : first >= 0xe0 && first <= 0xef ? 3
        : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    if (!length || length > text.size()) return 0;
    for (unsigned i = 1; i < length; ++i) if (bytes[i] < 0x80 || bytes[i] > 0xbf) return 0;
    if ((first == 0xe0 && bytes[1] < 0xa0) || (first == 0xed && bytes[1] > 0x9f)
        || (first == 0xf0 && bytes[1] < 0x90) || (first == 0xf4 && bytes[1] > 0x8f)) return 0;
    return length;
}

inline bool utf8(std::string_view text) noexcept {
    while (!text.empty()) {
        const auto count = character(text);
        if (!count) return false;
        text.remove_prefix(count);
    }
    return true;
}

template<std::size_t N>
bool copy(char (&output)[N], std::string_view input) noexcept {
    std::size_t written = 0;
    while (!input.empty() && written < N - 1) {
        const auto count = character(input);
        if (!count) {
            output[written++] = '?';
            input.remove_prefix(1);
            continue;
        }
        if (count > N - 1 - written) break;
        std::memcpy(output + written, input.data(), count);
        written += count;
        input.remove_prefix(count);
    }
    output[written] = '\0';
    return !input.empty();
}

inline es_status fail(es_error *error, es_status status, std::string_view message) noexcept {
    if (error) copy(error->message, message);
    return status;
}
}
#endif
