#ifndef ENGINE_SIM_FLEET_EXAMPLE_ADAPTER_HPP
#define ENGINE_SIM_FLEET_EXAMPLE_ADAPTER_HPP

#include <engine_sim/fleet.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace fleet_example {

enum class CallbackReply { next, complete, abort };
enum class CallbackCause { normal, fleet, period, protocol, backend };
struct StreamTimes {
    double input, current, dac;
};
struct CallbackRecord {
    StreamTimes times{};
    std::uint64_t flags = 0, first = ES_RT_NO_FRAME;
    std::size_t frames = 0;
    std::uint32_t pcm = 0, fallback = 0;
    std::array<std::uint32_t, 2> nonzero{};
    int status = ES_RT_STATE;
    CallbackCause cause = CallbackCause::normal;
    bool timestamped = false;
};

class Adapter {
public:
    static constexpr std::size_t deviceLimit = 1024;
    explicit Adapter(es_rt_fleet *fleet, std::uint32_t maximum, unsigned channels, std::uint64_t xrunMask,
                     std::uint64_t finish) : m_fleet(fleet), m_maximum(maximum), m_channels(channels),
                                              m_xrunMask(xrunMask), m_finish(finish) {
        if (!fleet || !maximum || maximum > deviceLimit || (channels != 1 && channels != 2))
            throw std::invalid_argument("Invalid Fleet device buffer shape");
        if (!m_head.is_lock_free() || !m_tail.is_lock_free() || !m_dropped.is_lock_free() || !m_stop.is_lock_free())
            throw std::runtime_error("Device observation atomics are not lock-free");
    }

    CallbackReply process(float *output, std::size_t frames, const StreamTimes *times,
                          std::uint64_t flags) noexcept {
        CallbackRecord note{};
        note.frames = frames;
        note.flags = flags;
        if (times) {
            note.timestamped = true;
            note.times = *times;
        }
        if (frames > deviceLimit || (frames && !output)) {
            // A backend violation can exceed the negotiated software bound.
            // Honor the host's actual buffer extent when it is representable.
            note.cause = CallbackCause::protocol;
            return silenceAndAbort(output, frames, note);
        }
        if (frames > m_maximum) {
            note.cause = CallbackCause::period;
            return silenceAndAbort(output, frames, note);
        }
        if ((flags & m_xrunMask) || m_stop.load(std::memory_order_acquire) != 0) {
            note.cause = CallbackCause::backend;
            return silenceAndAbort(output, frames, note);
        }

        const auto n = static_cast<std::uint32_t>(frames);
        auto *mono = m_channels == 1 ? output : m_mix.data();
        const es_rt_audio audio{sizeof(audio), ES_RT_V1, n, m_maximum, mono,
                                {m_stem[0].data(), m_stem[1].data()}};
        es_rt_block block{sizeof(block), ES_RT_V1, 0, 0, ES_RT_NO_FRAME, ES_RT_NO_FRAME};
        note.status = es_rt_process(m_fleet, &audio, &block);
        if (note.status != ES_RT_OK && note.status != ES_RT_BROKEN) {
            note.cause = CallbackCause::fleet;
            return silenceAndAbort(output, frames, note);
        }
        note.first = block.first;
        note.pcm = block.pcm_frames;
        note.fallback = block.fallback_frames;
        for (std::uint32_t i = 0; i < n; ++i) {
            if (m_channels == 2) {
                output[2 * i] = mono[i];
                output[2 * i + 1] = mono[i];
            }
            for (unsigned lane = 0; lane < note.nonzero.size(); ++lane) {
                if (m_stem[lane][i] != 0.f) ++note.nonzero[lane];
            }
        }
        if (note.status == ES_RT_BROKEN) {
            // Fleet already initialized this valid span and identified its prefix.
            note.cause = CallbackCause::fleet;
            m_stop.store(1, std::memory_order_release);
        }
        publish(note);
        if (note.status == ES_RT_BROKEN) return CallbackReply::abort;
        return block.first + n >= m_finish ? CallbackReply::complete : CallbackReply::next;
    }

    void requestStop() noexcept { m_stop.store(1, std::memory_order_release); }
    std::uint64_t dropped() const noexcept { return m_dropped.load(std::memory_order_relaxed); }
    bool poll(CallbackRecord &note) noexcept {
        const auto head = m_head.load(std::memory_order_relaxed);
        if (head == m_tail.load(std::memory_order_acquire)) return false;
        note = m_notes[head % m_notes.size()];
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

private:
    void publish(const CallbackRecord &note) noexcept {
        const auto tail = m_tail.load(std::memory_order_relaxed);
        const auto head = m_head.load(std::memory_order_acquire);
        if (tail == UINT64_MAX || tail - head >= m_notes.size()) {
            const auto count = m_dropped.load(std::memory_order_relaxed);
            if (count != UINT64_MAX) m_dropped.store(count + 1, std::memory_order_relaxed);
            return;
        }
        m_notes[tail % m_notes.size()] = note;
        m_tail.store(tail + 1, std::memory_order_release);
    }
    CallbackReply silenceAndAbort(float *output, std::size_t frames, const CallbackRecord &note) noexcept {
        if (output && frames <= std::numeric_limits<std::size_t>::max() / m_channels) {
            volatile float *samples = output;
            for (std::size_t i = 0; i < frames * m_channels; ++i) samples[i] = 0.f;
        }
        m_stop.store(1, std::memory_order_release);
        publish(note);
        return CallbackReply::abort;
    }

    es_rt_fleet *const m_fleet;
    const std::uint32_t m_maximum;
    const unsigned m_channels;
    const std::uint64_t m_xrunMask, m_finish;
    std::array<float, deviceLimit> m_mix{};
    std::array<std::array<float, deviceLimit>, 2> m_stem{};
    std::array<CallbackRecord, 256> m_notes{};
    alignas(64) std::atomic<std::uint64_t> m_head{0}, m_tail{0}, m_dropped{0};
    std::atomic<std::uint32_t> m_stop{0};
};

// Control-side correspondence between stream time, host monotonic time and the
// DAC estimate of the first sample in a recorded Fleet submission.
class Presentation {
public:
    explicit Presentation(unsigned rate) : m_rate(rate) {}

    bool calibrate(double before, double stream, double after) noexcept {
        m_valid = std::isfinite(before) && std::isfinite(stream) && std::isfinite(after) && after >= before;
        if (!m_valid) return false;
        offset = before + (after - before) / 2. - stream;
        uncertainty = (after - before) / 2.;
        return true;
    }

    bool observe(const CallbackRecord &note) noexcept {
        m_valid = m_valid && note.cause == CallbackCause::normal && note.timestamped &&
            std::isfinite(note.times.current) && std::isfinite(note.times.dac) &&
            note.first != ES_RT_NO_FRAME && note.frames <= UINT64_MAX - note.first &&
            note.times.dac >= note.times.current &&
            (!m_seen || (note.first == m_next && note.times.current >= m_current && note.times.dac >= m_dac));
        if (!m_valid) return false;
        if (m_seen) {
            frameResidual = (note.times.dac - m_dac) * m_rate - static_cast<double>(note.first - m_first);
        }
        m_first = note.first;
        m_next = note.first + note.frames;
        m_current = note.times.current;
        m_dac = note.times.dac;
        m_seen = true;
        return true;
    }

    double hostDacEstimate() const noexcept {
        return m_valid && m_seen ? m_dac + offset : std::numeric_limits<double>::quiet_NaN();
    }
    double offset = 0, uncertainty = 0, frameResidual = 0;

private:
    const unsigned m_rate;
    std::uint64_t m_first = 0, m_next = 0;
    double m_current = 0, m_dac = 0;
    bool m_seen = false, m_valid = false;
};

}
#endif
