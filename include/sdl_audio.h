#ifndef ENGINE_SIM_SDL_AUDIO_H
#define ENGINE_SIM_SDL_AUDIO_H

#include "desktop_host.h"
#include <memory>

struct SDL_AudioStream;
namespace desktop {
// Source PCM stays mono S16 at 44100 Hz. SDL owns only device conversion.
class SdlAudioOutput {
public:
    enum class Mode { Device, Virtual };
    struct Options {
        Mode mode = Mode::Device;
        // Internal fault/observation seams; null entries use the real SDL API.
        bool (*resume)(SDL_AudioStream *) = nullptr;
        bool (*put)(SDL_AudioStream *, const void *, int) = nullptr;
        int (*queued)(SDL_AudioStream *) = nullptr;
        void (*observeSource)(void *, const std::int16_t *, std::size_t) noexcept = nullptr;
        void *observer = nullptr;
    };
    struct DeviceInfo {
        std::string driver, name;
        int sourceFrequency = 44100, sourceChannels = 1;
        unsigned sourceFormat = 0;
        int deviceFrequency = 0, deviceChannels = 0, deviceQuantum = 0;
        unsigned deviceFormat = 0;
        unsigned logicalDevice = 0;
        bool virtualDevice = false;
    };
    struct Statistics {
        std::uint64_t submittedSource = 0, pumpedSource = 0, discardedSource = 0;
        std::uint64_t attemptedFrames = 0, putFrames = 0, consumedInput = 0;
        std::uint64_t primingSilence = 0, starvationSilence = 0;
        std::uint64_t starvationBlocks = 0, overflows = 0, latePolls = 0;
        std::uint64_t maxQueuedFrames = 0, minQueuedFrames = 0, maxSourceFrames = 0;
        std::uint64_t joinNanoseconds = 0;
        std::uint64_t consumedSource = 0, pauseSilence = 0, retiredSource = 0, epoch = 0;
    };
    static constexpr AudioOffset Capacity = 44100, Watermark = 4410, Block = 1024;
    SdlAudioOutput();
    ~SdlAudioOutput();
    SdlAudioOutput(const SdlAudioOutput &) = delete;
    SdlAudioOutput &operator=(const SdlAudioOutput &) = delete;
    void open(const Options &options);
    void setPlaying(bool playing);
    void quiesce() noexcept; // Join before the simulator or stream can be released.
    void resetQueue();
    void setSourcePaused(bool paused);
    void shutdown() noexcept;
    void check() const;
    AudioOffset cursor() const; // SDL input consumed, not a physical playback clock.
    AudioProgress progress() const;
    AudioSegments lock(AudioOffset offset, AudioOffset count);
    void unlock(const AudioSegments &segments);
    DeviceInfo deviceInfo() const;
    Statistics statistics() const;
    bool running() const;
private:
    struct State;
    std::unique_ptr<State> state;
};
}
#endif
