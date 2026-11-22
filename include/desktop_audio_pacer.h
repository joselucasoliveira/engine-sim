#ifndef ENGINE_SIM_DESKTOP_AUDIO_PACER_H
#define ENGINE_SIM_DESKTOP_AUDIO_PACER_H

#include "desktop_host.h"

#include <array>
#include <functional>

namespace desktop {
// The owner renders source PCM; the device callback only consumes prepared PCM.
class AudioPacer {
public:
    static constexpr std::uint32_t Rate = 44100, Target = 4410, Capacity = 4410;
    struct Statistics {
        std::uint64_t requestedDevice = 0, generatedDevice = 0, submitted = 0;
        std::uint64_t requestedSource = 0, generatedSource = 0;
        std::uint64_t consumed = 0, consumedSource = 0, discarded = 0;
        std::uint64_t starvationSilence = 0, pauseSilence = 0, primingSilence = 0;
        std::uint64_t ready = 0, reserved = 0, deviceQueued = 0, maximumQueued = 0;
        std::uint64_t deviceUnderruns = 0, sourceFaults = 0, rejectedManualSteps = 0;
        std::uint64_t epoch = 0;
    };
    struct Observation {
        void (*ready)(void *, const std::int16_t *, std::size_t) noexcept = nullptr;
        void *context = nullptr;
    };
    explicit AudioPacer(std::function<float()> source);
    AudioPacer(const AudioPacer &) = delete;
    AudioPacer &operator=(const AudioPacer &) = delete;
    bool service(Host &device, unsigned speedDivisor, bool paused, std::uint32_t manualFrames = 0);
    void setObservation(Observation value) { m_observation = value; }
    const Statistics &statistics() const { return m_stats; }
    const char *fault() const { return m_error.data(); }
    std::uint32_t phase() const { return m_phase; }

private:
    AudioProgress progress(Host &device);
    void submit(Host &device, AudioProgress &progress);
    void produce(std::uint32_t frames, unsigned divisor);
    float sourceFrame();
    std::int16_t deviceFrame(unsigned divisor);
    std::function<float()> m_source;
    Observation m_observation;
    Statistics m_stats;
    std::array<std::int16_t, Capacity> m_ready{};
    std::array<char, 512> m_error{};
    std::size_t m_begin = 0, m_count = 0;
    std::uint32_t m_writeCursor = 0, m_previousCursor = 0, m_phase = 0;
    std::uint64_t m_fallbackConsumed = 0, m_fallbackWritten = 0;
    float m_previous = 0, m_current = 0;
    bool m_haveDevice = false, m_haveSource = false, m_nextSource = false;
    bool m_shifted = false, m_faulted = false;
};
}
#endif
