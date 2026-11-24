#include "owner.hpp"

#include <algorithm>
#include <limits>

namespace engine_sim::fleet {
namespace {
void copyPlane(float *out, const float *in, std::uint32_t count) noexcept {
    // Explicit stores keep the callback independent of lazy CRT copy/clear helpers.
    volatile float *destination = out;
    for (std::uint32_t i = 0; i < count; ++i) destination[i] = in[i];
}

void clearPlane(float *out, std::uint32_t count) noexcept {
    volatile float *destination = out;
    for (std::uint32_t i = 0; i < count; ++i) destination[i] = 0.0f;
}
}

bool PcmTransport::tryPublish(const PcmTile &tile) noexcept {
    const auto write = m_write.load(std::memory_order_relaxed);
    const auto read = m_read.load(std::memory_order_acquire);
    if (write == std::numeric_limits<std::uint64_t>::max() || write - read == m_count || broken())
        return false;
    m_slots[write % m_count] = tile;
    m_write.store(write + 1, std::memory_order_release);
    return true;
}

std::uint32_t PcmTransport::consume(std::uint64_t epoch, std::uint64_t first, std::uint32_t lanes,
                                   const es_rt_audio &audio) noexcept {
    std::uint32_t delivered = 0;
    if (m_broken.load(std::memory_order_relaxed) == 0) {
        const auto published = m_write.load(std::memory_order_acquire);
        const auto previous = m_read.load(std::memory_order_relaxed);
        auto read = previous;
        auto offset = m_offset;
        while (delivered < audio.frames && read != published) {
            const auto &tile = m_slots[read % m_count];
            if (tile.epoch != epoch || tile.first != first + delivered - offset) break;
            const auto count = std::min(audio.frames - delivered, tileFrames - offset);
            if (audio.mix) copyPlane(audio.mix + delivered, tile.mix.data() + offset, count);
            for (std::uint32_t lane = 0; lane < lanes; ++lane) {
                if (audio.stem[lane])
                    copyPlane(audio.stem[lane] + delivered, tile.stem[lane].data() + offset, count);
            }
            delivered += count;
            offset += count;
            if (offset == tileFrames) {
                ++read;
                offset = 0;
            }
        }
        m_offset = offset;
        if (read != previous) m_read.store(read, std::memory_order_release);
    }
    if (delivered < audio.frames) {
        const auto remaining = audio.frames - delivered;
        if (audio.mix) clearPlane(audio.mix + delivered, remaining);
        for (std::uint32_t lane = 0; lane < lanes; ++lane) {
            if (audio.stem[lane]) clearPlane(audio.stem[lane] + delivered, remaining);
        }
        // A worker acquiring the subsequent fallback D must also see this break.
        m_broken.store(1, std::memory_order_release);
    }
    return delivered;
}

}

extern "C" std::int32_t ES_RT_CALL es_rt_process(es_rt_fleet *fleet, const es_rt_audio *audio,
                                                es_rt_block *result) {
    if (!fleet || !audio || !result) return ES_RT_INVALID;
    using engine_sim::fleet::prefix;
    if (const auto status = prefix(*audio); status != ES_RT_OK) return status;
    if (const auto status = prefix(*result); status != ES_RT_OK) return status;

    const auto frames = audio->frames;
    const auto lanes = fleet->config.lanes;
    if (frames > fleet->config.max_frames || frames > audio->capacity) return ES_RT_CAPACITY;
    bool hasOutput = audio->mix != nullptr;
    for (std::uint32_t lane = 0; lane < 8; ++lane) {
        if (!audio->stem[lane]) continue;
        if (lane >= lanes) return ES_RT_INVALID;
        hasOutput = true;
    }
    if (frames && !hasOutput) return ES_RT_INVALID;
    auto &submitted = fleet->commands.submitted;
    const auto first = submitted.load(std::memory_order_relaxed);
    if (frames > std::numeric_limits<std::uint64_t>::max() - first) return ES_RT_CAPACITY;

    std::uint32_t delivered = 0;
    if (frames) {
        delivered = fleet->pcm.consume(fleet->config.epoch, first, lanes, *audio);
        submitted.store(first + frames, std::memory_order_release);
    }
    result->epoch = fleet->config.epoch;
    result->first = first;
    result->pcm_frames = delivered;
    result->fallback_frames = frames - delivered;
    return delivered == frames ? ES_RT_OK : ES_RT_BROKEN;
}
