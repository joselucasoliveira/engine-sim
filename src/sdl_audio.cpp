#include "sdl_audio.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace desktop {
struct SdlAudioOutput::State {
    Options options;
    DeviceInfo info;
    SDL_AudioStream *stream = nullptr;
    bool initialized = false, locked = false;
    AudioSegments span;
    std::thread::id owner;
    std::thread pump;
    std::atomic<bool> stop{true}, failed{false};
    std::atomic<bool> removed{false}, lost{false};
    std::array<SDL_AudioDeviceID, 256> endpointCandidates{};
    std::size_t endpointCount = 0;
    bool watching = false;
    char error[256]{};
    std::mutex waitMutex;
    std::condition_variable wake;
    std::array<std::int16_t, Capacity> staging{}, fifo{};
    std::array<std::int16_t, Block> block{};
    std::atomic<std::uint64_t> head{0}, tail{0};
    std::atomic<std::uint64_t> submitted{0}, pumped{0}, discarded{0}, attempted{0}, put{0}, consumed{0};
    std::atomic<std::uint64_t> priming{0}, silence{0}, starvations{0}, overflows{0}, late{0};
    std::atomic<std::uint64_t> maxQueue{0}, minQueue{Watermark}, maxSource{0}, joinTime{0};
    static bool watch(void *context, SDL_Event *event) {
        auto &s = *static_cast<State *>(context);
        if (event->type == SDL_EVENT_AUDIO_DEVICE_REMOVED && !event->adevice.recording) {
            const bool endpoint = std::find(s.endpointCandidates.begin(), s.endpointCandidates.begin() + s.endpointCount,
                                            event->adevice.which) != s.endpointCandidates.begin() + s.endpointCount;
            if (event->adevice.which == s.info.logicalDevice || endpoint) {
                s.lost.store(true); s.stop.store(true); s.wake.notify_all();
            } else s.removed.store(true);
        }
        return true;
    }
    void requireOwner() const {
        if (owner != std::this_thread::get_id()) throw DesktopError("audio", "operation requires its opening thread");
    }
    void fail(const char *operation) noexcept {
        // Only the pump writes this buffer. The release publishes the complete message.
        std::snprintf(error, sizeof(error), "%s: %s", operation, SDL_GetError());
        failed.store(true, std::memory_order_release);
        stop.store(true, std::memory_order_release);
    }
    bool putData(const void *data, int frames) {
        attempted.fetch_add(frames, std::memory_order_relaxed);
        if (!(options.put ? options.put(stream, data, frames * 2) : SDL_PutAudioStreamData(stream, data, frames * 2))) return false;
        put.fetch_add(frames, std::memory_order_relaxed);
        return true;
    }
    void run() noexcept {
        using Clock = std::chrono::steady_clock;
        auto previous = Clock::now();
        while (!stop.load(std::memory_order_acquire)) {
            const auto now = Clock::now();
            if (now - previous > std::chrono::milliseconds(100)) late.fetch_add(1, std::memory_order_relaxed);
            previous = now;
            const int bytes = options.queued ? options.queued(stream) : SDL_GetAudioStreamQueued(stream);
            if (bytes < 0 || bytes % 2 != 0) { fail("query stream queue"); break; }
            const auto queued = static_cast<std::uint64_t>(bytes / 2);
            const auto total = put.load(std::memory_order_relaxed);
            if (queued > total || queued > Watermark) { SDL_SetError("invalid bounded input queue"); fail("query stream queue"); break; }
            consumed.store(total - queued, std::memory_order_release);
            minQueue.store(std::min(minQueue.load(), queued));
            maxQueue.store(std::max(maxQueue.load(), queued));
            if (queued < Watermark) {
                const auto count = std::min<std::uint64_t>(Block, Watermark - queued);
                const auto read = tail.load(std::memory_order_relaxed);
                const auto available = head.load(std::memory_order_acquire) - read;
                const auto source = std::min(count, available);
                for (std::uint64_t i = 0; i < source; ++i) block[i] = fifo[(read + i) % Capacity];
                std::fill(block.begin() + source, block.begin() + count, 0);
                tail.store(read + source, std::memory_order_release);
                if (!putData(block.data(), static_cast<int>(count))) {
                    discarded.fetch_add(source); fail("feed stream"); break;
                }
                pumped.fetch_add(source);
                if (options.observeSource && source) options.observeSource(options.observer, block.data(), source);
                if (source < count) { silence.fetch_add(count - source); starvations.fetch_add(1); }
                maxQueue.store(std::max(maxQueue.load(), queued + count));
            } else {
                std::unique_lock<std::mutex> lock(waitMutex);
                wake.wait_for(lock, std::chrono::milliseconds(1), [this] { return stop.load(); });
            }
        }
        // std::thread does not run SDL's thread wrapper, including its TLS cleanup.
        // Failed stream calls can allocate an SDL error buffer on this thread.
        SDL_CleanupTLS();
    }
};
SdlAudioOutput::SdlAudioOutput() : state(std::make_unique<State>()) {}
SdlAudioOutput::~SdlAudioOutput() { shutdown(); }
void SdlAudioOutput::open(const Options &options) {
    if (state->initialized || state->stream) throw DesktopError("audio", "already open");
    // A fresh state also resets telemetry after a complete close/reopen.
    state = std::make_unique<State>();
    auto &s = *state;
    s.owner = std::this_thread::get_id(); s.options = options;
    try {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) throw DesktopError("audio initialization", SDL_GetError());
        s.initialized = true;
        const char *driver = SDL_GetCurrentAudioDriver();
        if (!driver) throw DesktopError("query audio driver", SDL_GetError());
        s.info.driver = driver;
        s.info.virtualDevice = s.info.driver == "dummy" || s.info.driver == "disk";
        if ((options.mode == Mode::Virtual) != s.info.virtualDevice)
            throw DesktopError("audio", "selected driver does not match the explicit device/virtual mode");
        SDL_AudioSpec source{SDL_AUDIO_S16, 1, 44100};
        s.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &source, nullptr, nullptr);
        if (!s.stream) throw DesktopError("open audio stream", SDL_GetError());
        const auto device = SDL_GetAudioStreamDevice(s.stream);
        s.info.logicalDevice = device;
        SDL_AudioSpec destination{};
        if (!device || !SDL_GetAudioDeviceFormat(device, &destination, &s.info.deviceQuantum))
            throw DesktopError("query audio device", SDL_GetError());
        const char *name = SDL_GetAudioDeviceName(device);
        if (!name) throw DesktopError("query audio device name", SDL_GetError());
        s.info.name = name; s.info.sourceFormat = source.format;
        s.info.deviceFrequency = destination.freq; s.info.deviceChannels = destination.channels; s.info.deviceFormat = destination.format;
        int count = 0;
        auto *devices = SDL_GetAudioPlaybackDevices(&count);
        if (!devices) throw DesktopError("enumerate audio endpoints", SDL_GetError());
        if (count > static_cast<int>(s.endpointCandidates.size())) {
            SDL_free(devices); throw DesktopError("audio", "too many physical endpoints for the bounded device watch");
        }
        // SDL 3.2 does not expose the physical ID behind a logical default. Track
        // all same-name candidates and fail conservatively on their removal.
        for (int i = 0; i < count; ++i) {
            const char *physicalName = SDL_GetAudioDeviceName(devices[i]);
            if (physicalName && s.info.name == physicalName) s.endpointCandidates[s.endpointCount++] = devices[i];
        }
        SDL_free(devices);
        if (!SDL_AddEventWatch(State::watch, &s)) throw DesktopError("watch audio device", SDL_GetError());
        s.watching = true;
        // Match the original 100 ms ring lead. Initial silence is counted separately.
        std::array<std::int16_t, Watermark> silence{};
        if (!s.putData(silence.data(), Watermark)) throw DesktopError("prime audio stream", SDL_GetError());
        s.priming.store(Watermark); s.maxQueue.store(Watermark);
    } catch (...) { shutdown(); throw; }
}
void SdlAudioOutput::setPlaying(bool playing) {
    auto &s = *state; s.requireOwner(); check();
    if (!s.stream) throw DesktopError("audio", "not open");
    if (!playing) { quiesce(); return; }
    if (s.pump.joinable()) return;
    s.stop.store(false, std::memory_order_release);
    try {
        s.pump = std::thread([&s] { s.run(); });
        if (!(s.options.resume ? s.options.resume(s.stream) : SDL_ResumeAudioStreamDevice(s.stream)))
            throw DesktopError("resume audio stream", SDL_GetError());
    } catch (...) { quiesce(); throw; }
}
void SdlAudioOutput::quiesce() noexcept {
    auto &s = *state;
    const auto start = std::chrono::steady_clock::now();
    s.stop.store(true, std::memory_order_release); s.wake.notify_all();
    if (s.stream) SDL_PauseAudioStreamDevice(s.stream);
    if (s.pump.joinable()) s.pump.join();
    s.joinTime.store(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()));
}
void SdlAudioOutput::shutdown() noexcept {
    quiesce(); auto &s = *state;
    if (s.watching) { SDL_RemoveEventWatch(State::watch, &s); s.watching = false; }
    if (s.stream) { SDL_DestroyAudioStream(s.stream); s.stream = nullptr; }
    if (s.initialized) { SDL_QuitSubSystem(SDL_INIT_AUDIO); s.initialized = false; }
    s.locked = false;
}
void SdlAudioOutput::check() const {
    auto &s = *state;
    if (s.removed.exchange(false)) {
        // SDL keeps disconnected defaults consuming through a silent zombie device.
        // On the owner thread, verify that the logical endpoint still has a live
        // physical namesake. Unrelated removals do not interrupt this stream.
        s.requireOwner();
        int count = 0;
        auto *devices = SDL_GetAudioPlaybackDevices(&count);
        const char *logicalName = SDL_GetAudioDeviceName(s.info.logicalDevice);
        bool connected = false;
        for (int i = 0; devices && i < count; ++i) {
            const char *name = SDL_GetAudioDeviceName(devices[i]);
            if (logicalName && name && std::strcmp(name, logicalName) == 0) connected = true;
        }
        SDL_free(devices);
        if (!connected) { s.lost.store(true); s.stop.store(true); s.wake.notify_all(); }
    }
    if (s.lost.load()) throw DesktopError("audio device", "playback endpoint was disconnected");
    if (s.failed.load(std::memory_order_acquire)) throw DesktopError("audio pump", s.error);
}
AudioOffset SdlAudioOutput::cursor() const { check(); return static_cast<AudioOffset>(state->consumed.load(std::memory_order_acquire) % Capacity); }
AudioSegments SdlAudioOutput::lock(AudioOffset offset, AudioOffset count) {
    auto &s = *state; s.requireOwner(); check();
    if (!s.stream || s.locked || offset >= Capacity || count > Capacity) throw DesktopError("audio", "invalid or nested source span");
    const auto first = std::min(count, Capacity - offset);
    s.span = {s.staging.data() + offset, s.staging.data(), first, count - first}; s.locked = true;
    return s.span;
}
void SdlAudioOutput::unlock(const AudioSegments &span) {
    auto &s = *state; s.requireOwner(); check();
    if (!s.locked || span.first != s.span.first || span.second != s.span.second || span.firstCount != s.span.firstCount || span.secondCount != s.span.secondCount)
        throw DesktopError("audio", "source span does not match the active lock");
    s.locked = false;
    const auto count = static_cast<std::uint64_t>(span.firstCount) + span.secondCount;
    const auto write = s.head.load(std::memory_order_relaxed);
    if (count > Capacity - (write - s.tail.load(std::memory_order_acquire))) {
        s.overflows.fetch_add(1); throw DesktopError("audio", "bounded source queue is full");
    }
    for (std::uint64_t i = 0; i < count; ++i) s.fifo[(write + i) % Capacity] = i < span.firstCount ? span.first[i] : span.second[i - span.firstCount];
    s.submitted.fetch_add(count);
    s.head.store(write + count, std::memory_order_release);
    s.maxSource.store(std::max(s.maxSource.load(), write + count - s.tail.load(std::memory_order_acquire)));
}
SdlAudioOutput::DeviceInfo SdlAudioOutput::deviceInfo() const { return state->info; }
SdlAudioOutput::Statistics SdlAudioOutput::statistics() const {
    const auto &s = *state;
    return {s.submitted.load(), s.pumped.load(), s.discarded.load(), s.attempted.load(), s.put.load(), s.consumed.load(),
            s.priming.load(), s.silence.load(), s.starvations.load(), s.overflows.load(), s.late.load(),
            s.maxQueue.load(), s.minQueue.load(), s.maxSource.load(), s.joinTime.load()};
}
bool SdlAudioOutput::running() const { return !state->stop.load(std::memory_order_acquire); }
}
