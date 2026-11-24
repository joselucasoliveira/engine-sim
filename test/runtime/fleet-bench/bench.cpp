#include <engine_sim/fleet.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfenv>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(ENGINE_SIM_FLEET_BENCH_PORTAUDIO)
#include <portaudio.h>
#include <iomanip>
#endif

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#endif

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Ns = std::chrono::nanoseconds;
using namespace std::chrono_literals;
constexpr std::uint64_t q = 64, epoch = 1;
constexpr unsigned maximum = 1024;
constexpr unsigned maxLanes = 8;
constexpr auto digestBasis = UINT64_C(14695981039346656037);
constexpr auto digestPrime = UINT64_C(1099511628211);
using Owner = std::unique_ptr<es_rt_fleet, decltype(&es_rt_destroy)>;

void require(bool valid, const char *error) {
    if (!valid) throw std::runtime_error(error);
}
template<class Record> Record init() {
    Record out{};
    out.size = sizeof(out);
    out.abi = ES_RT_V1;
    return out;
}

std::uint64_t ns(Clock::time_point time = Clock::now()) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<Ns>(time.time_since_epoch()).count());
}
void bind(int cpu) {
    if (cpu < 0) return;
#if defined(__linux__)
    require(cpu < CPU_SETSIZE, "CPU selection outside native affinity set");
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    require(pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0, "cannot bind native host CPU");
#else
    require(false, "hard affinity not available on this host");
#endif
}

struct Settings {
    fs::path path, single, singleRoot, driven, drivenRoot;
    unsigned block, rate, profile;
    std::uint64_t frames;
    int audioCore, loadCore;
    bool live, pace, variable, injectMiss, injectStarve, replaySmoke;
    std::string roster = "FD";
    int device = -1;
    const char *deviceName = nullptr;
    fs::path ls;

    bool free(unsigned lane) const noexcept { return roster[lane] == 'F'; }
};
template<class Number> Number number(std::string_view text) {
    Number out{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    require(result.ec == std::errc{} && result.ptr == text.data() + text.size(), "Invalid native benchmark argument");
    return out;
}
struct ClockSample {
    std::uint64_t first, due, entered, returned, finished;
    std::uint32_t pcm, fallback;
    std::int32_t status;
    std::uint32_t zero = 0;
};
static_assert(sizeof(ClockSample) == 56);

#if defined(ENGINE_SIM_FLEET_BENCH_PORTAUDIO)
struct DeviceSample {
    std::uint64_t first, frames, flags;
    double adc, current, dac;
};
static_assert(sizeof(DeviceSample) == 48);
constexpr auto xruns = paInputUnderflow | paInputOverflow | paOutputUnderflow | paOutputOverflow;

void checkPa(PaError code, const char *operation) {
    if (code != paNoError) throw std::runtime_error(std::string(operation) + ": " + Pa_GetErrorText(code));
}
struct PortAudio {
    PortAudio() { checkPa(Pa_Initialize(), "PortAudio initialization"); }
    ~PortAudio() { (void)Pa_Terminate(); }
};
#endif

struct Event {
    enum Kind { vector, prepare, swap, release, gain, activate } kind;
    std::uint64_t fire;
    es_rt_vector controls{};
    unsigned lane = 0, seed = 0;
    double gainTarget = 0;
};
struct LoggedEvent {
    es_rt_event event{};
    std::uint64_t trigger, sampled, submitted;
    std::uint64_t receiptAt = 0, end = ES_RT_NO_FRAME;
    int issue = ES_RT_OK;
    std::uint32_t result = ES_RT_PENDING;
    std::uint64_t applied = ES_RT_NO_FRAME;
};
struct Window {
    Window() { hash.fill(digestBasis); }
    std::array<std::uint64_t, maxLanes> nonzero{}, hash;
    std::uint64_t frames = 0;
};
struct GameUpdate { std::uint64_t due, observed, submitted, generated, sealed; };
struct VoicePoint { std::uint64_t generated; es_rt_lane_info info; };
struct Metrics {
    Metrics() { hash.fill(digestBasis); }
    std::uint64_t deadlineMisses = 0, breaks = 0, fallback = 0, late = 0, faults = 0, overlaps = 0, unsubmitted = 0;
    std::uint64_t invalidSamples = 0, clipped = 0, elapsed = 0, prepMax = 0;
    std::uint64_t lookahead = 0, cpuIterations = 0, startupNs = 0;
    std::uint64_t audioCpuNs = 0, lowLead = UINT64_MAX, highLead = 0, queueRejects = 0, droppedSnapshots = 0;
    std::array<std::uint64_t, maxLanes> ticks{}, hash;
    float mixPeak = 0;
    int audioRounding = -1, audioFinalRounding = -1;
};
std::vector<Event> script(const Settings &cfg) {
    std::vector<Event> events;
    for (unsigned lane = 0; lane < cfg.roster.size(); ++lane) {
        const auto start = (lane / 2) * cfg.rate / 5;
        if (start) events.push_back({Event::activate, start, {}, lane, 403 + lane});
        if (cfg.free(lane)) events.push_back({Event::vector, start + (cfg.rate + 2) / 3, {.55, 0., 0., 1, 0}, lane});
        else events.push_back({Event::vector, start + cfg.rate / 10, {.25, 0., 600., 1, 0}, lane});
    }
    const auto replacement = cfg.roster.find('F');
    const auto replaceLane = static_cast<unsigned>(replacement);
    for (unsigned i = 0; std::uint64_t(i) * 60 * cfg.rate < cfg.frames; ++i) {
        const std::uint64_t base = std::uint64_t(i) * cfg.rate * 60;
        const auto offset = [&](unsigned sec) { return base + cfg.rate * sec; };
        for (unsigned lane = 0; lane < cfg.roster.size(); ++lane) {
            if (cfg.free(lane)) {
                events.push_back({Event::vector, offset(10), {.45, 0., 0., 1, 0}, lane});
                events.push_back({Event::vector, offset(20), {.55, 0., 0., 1, 0}, lane});
            } else {
                events.push_back({Event::vector, offset(12), {.25, 5., 800., 1, 0}, lane});
                events.push_back({Event::vector, offset(22), {.25, 0., 600., 1, 0}, lane});
            }
        }
        if (replacement != std::string::npos) {
            const auto seed = 403u + static_cast<unsigned>(cfg.roster.size()) + i;
            events.push_back({Event::prepare, offset(28), {}, replaceLane, seed});
            events.push_back({Event::swap, offset(30), {}, replaceLane});
            events.push_back({Event::release, offset(30) + (cfg.rate + 2) / 3, {.55, 0., 0., 1, 0}, replaceLane});
        }
    }
    if (cfg.replaySmoke) {
        const auto change = cfg.rate * 4 / 5 + 1;
        events.push_back({Event::prepare, cfg.rate * 3 / 5, {}, 0, 405});
        events.push_back({Event::swap, change, {}, 0});
        events.push_back({Event::release, change + (cfg.rate + 2) / 3, {.55, 0., 0., 1, 0}, 0});
    }
    if (cfg.live || cfg.replaySmoke) {
        unsigned i = 0;
        for (std::uint64_t frame = cfg.rate / 4; frame < cfg.frames; frame += cfg.rate / 4) {
            events.push_back({Event::gain, frame, {}, 0, 0, (++i % 2) ? .8 : 1.});
        }
    }
    const auto cutoff = cfg.frames > 2 * cfg.block + 64 ? cfg.frames - (2 * cfg.block + 64) : 0;
    events.erase(std::remove_if(events.begin(), events.end(), [&](const Event &e) { return e.fire >= cutoff; }), events.end());
    std::stable_sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
        if (a.fire == b.fire) return a.kind != Event::prepare && b.kind == Event::prepare;
        return a.fire < b.fire;
    });
    return events;
}

std::uint64_t kernel(std::uint64_t value, std::uint64_t count) noexcept {
    for (std::uint64_t i = 0; i < count; ++i) value = value * UINT64_C(6364136223846793005) + (i | 1);
    return value;
}

class Trial {
public:
    explicit Trial(Settings setup)
        : settings(std::move(setup)), agenda(script(settings)), windows((settings.frames + settings.rate - 1) / settings.rate) {
        const auto smallest = settings.variable ? settings.block / 2 : settings.block;
        records.reserve(static_cast<std::size_t>((settings.frames + smallest - 1) / smallest));
#if defined(ENGINE_SIM_FLEET_BENCH_PORTAUDIO)
        if (settings.device >= 0) {
            const auto count = static_cast<std::size_t>(settings.frames / settings.block);
            records.resize(count);
            records.clear();
            deviceRecords.resize(count);
            deviceRecords.clear();
        }
#endif

        events.reserve(agenda.size() + 2);
        updates.reserve(settings.live ? static_cast<std::size_t>(settings.frames * 60 / settings.rate + 1) : 0);
        voicePoints.reserve((windows.size() + 16) * settings.roster.size());
    }

    bool run() {
        if (const auto *cpu = std::getenv("ES_FLEET_COORDINATOR_CORE")) bind(number<int>(cpu));
        auto opts = init<es_rt_config>();
        opts.rate = settings.rate;
        opts.lanes = static_cast<std::uint32_t>(settings.roster.size());
        opts.epoch = epoch;
        opts.profile = settings.profile;
        opts.max_frames = settings.block;
        es_rt_fleet *value = nullptr;
        auto diagnostic = init<es_rt_diagnostic>();
        require(es_rt_create(&opts, &value, &diagnostic) == ES_RT_OK, diagnostic.message);
        fleet.reset(value);

        for (unsigned lane = 0; lane < std::min(opts.lanes, 2u); ++lane) {
            auto event = command(ES_RT_ACTIVATE, 0);
            event.next_voice = {1, lane, 0};
            event.duration_ms = ES_RT_DFLT_MS;
            event.candidate = candidate(lane, 403 + lane);
            require(event.candidate.epoch == epoch && send(event, 0, 0, 0), "initial activation");
        }
        const auto priming = ns();
        require(es_rt_start(fleet.get(), &diagnostic) == ES_RT_OK, diagnostic.message);
        checkAcks();
        metrics.startupNs = ns() - priming;
        const auto advertised = position();
        high = advertised.high_frames;
        require(advertised.generated == high && advertised.submitted == 0, "initial H was not retained");

        std::exception_ptr audioFailure, loadFailure;
        std::thread load;
        if (settings.live) {
            load = std::thread([&] {
                try {
                    bind(settings.loadCore);
                    std::uint64_t value = 1;
                    while (!stop.load(std::memory_order_relaxed))
                        value = kernel(value, 2048);
                    loadDigest = value;
                } catch (...) {
                    loadFailure = std::current_exception();
                    stop.store(true, std::memory_order_release);
                }
            });
        }
        auto shutdown = [&] {
            stop.store(true, std::memory_order_release);
            if (load.joinable()) load.join();
        };
        std::thread audio;
        try {
#if defined(ENGINE_SIM_FLEET_BENCH_PORTAUDIO)
            if (settings.device >= 0) {
                runDevice();
            } else
#endif
            {
                audio = std::thread([&] {
                    try { pump(); }
                    catch (...) {
                        audioFailure = std::current_exception();
                        done.store(true, std::memory_order_release);
                    }
                });
                coordinate();
                audio.join();
            }
            shutdown();
        } catch (...) {
            shutdown();
            if (audio.joinable()) audio.join();
            throw;
        }
        require(es_rt_quiesce(fleet.get()) == ES_RT_OK, "benchmark quiesce");
        checkAcks();
        capture();
        if (audioFailure) std::rethrow_exception(audioFailure);
        if (loadFailure) std::rethrow_exception(loadFailure);
        return !deviceXruns && !metrics.deadlineMisses && !metrics.breaks && !metrics.fallback && !metrics.late && !metrics.unsubmitted &&
               !metrics.faults && !metrics.invalidSamples && ended == settings.frames &&
               std::all_of(metrics.hash.begin(), metrics.hash.begin() + settings.roster.size(),
                           [](auto hash) { return hash != 0; });
    }

private:
    es_rt_event command(unsigned kind, std::uint64_t frame) const {
        auto event = init<es_rt_event>();
        event.epoch = epoch;
        event.sequence = events.size() + 1;
        event.kind = kind;
        event.frame = frame;
        return event;
    }
    es_rt_token candidate(unsigned lane, unsigned seed) {
        const auto start = ns();
        auto voice = init<es_rt_voice_config>();
        const auto free = settings.free(lane);
        voice.mode = free ? ES_RT_FREE : ES_RT_DRIVEN;
        voice.controls_policy = ES_RT_GAME;
        voice.seed = seed;
        voice.initial = {.25, 0., 0., 1, free ? 1u : 0u};
        const auto path = (free ? settings.single : settings.roster[lane] == 'L' ? settings.ls : settings.driven).u8string();
        const auto root = (free ? settings.singleRoot : settings.drivenRoot).u8string();
        es_rt_token token{};
        auto detail = init<es_rt_diagnostic>();
        const auto status = es_rt_prepare_file(fleet.get(), &voice, path.c_str(), root.c_str(), &token, &detail);
        metrics.prepMax = std::max(metrics.prepMax, ns() - start);
        if (status != ES_RT_OK) {
            std::cerr << "prepare status=" << status << " " << detail.message << '\n';
            ++metrics.faults;
        }
        return token;
    }
    es_rt_transport position() const {
        auto value = init<es_rt_transport>();
        require(es_rt_get_transport(fleet.get(), &value) == ES_RT_OK, "benchmark transport status");
        return value;
    }
    bool send(const es_rt_event &event, std::uint64_t trigger, std::uint64_t sampled, std::uint64_t d) {
        const auto status = es_rt_submit(fleet.get(), &event);
        events.push_back({event, trigger, sampled, d, 0, ES_RT_NO_FRAME, status});
        if (status != ES_RT_OK) {
            ++metrics.late;
            return false;
        }
        outstanding.push_back(events.size() - 1);
        if (sampled) metrics.lookahead = std::max(metrics.lookahead, event.frame - d);
        return true;
    }
    void checkAcks() {
        for (auto at = outstanding.begin(); at != outstanding.end();) {
            auto &event = events[*at];
            auto reply = init<es_rt_result>();
            const auto status = es_rt_poll_result(fleet.get(), event.event.sequence, &reply);
            if (status == ES_RT_PENDING) { ++at; continue; }
            if (status == ES_RT_OK) {
                event.result = reply.reason;
                event.receiptAt = ns();
                event.end = reply.ended;
                event.applied = reply.applied;
                if (reply.reason == ES_RT_OK && reply.kind == ES_RT_REPLACE) ++metrics.overlaps;
                else if (reply.reason != ES_RT_OK) ++metrics.late;
            } else {
                ++metrics.late;
            }
            at = outstanding.erase(at);
        }
        es_rt_token token{};
        es_rt_lane_info info{};
        while (es_rt_collect(fleet.get(), &token, &info) == ES_RT_OK) {
            retired.push_back(info);
            if (info.fault) ++metrics.faults;
        }
        auto snapshot = init<es_rt_snapshot>();
        for (unsigned i = 0; i < 16 && es_rt_poll_snapshot(fleet.get(), &snapshot) == ES_RT_OK; ++i) {
            for (unsigned lane = 0; lane < settings.roster.size(); ++lane) {
                const auto &voice = snapshot.lane[lane];
                metrics.ticks[lane] = std::max(metrics.ticks[lane], voice.ticks);
                if (voice.fault && !seenFault[lane]) {
                    seenFault[lane] = true;
                    ++metrics.faults;
                }
                const auto second = snapshot.generated / settings.rate;
                if (voice.voice.generation && (voice.voice.generation != lastGeneration[lane] || second != lastSecond[lane])) {
                    voicePoints.push_back({snapshot.generated, voice});
                    lastGeneration[lane] = voice.voice.generation;
                    lastSecond[lane] = second;
                }
            }
        }
    }

    void coordinate() {
        std::size_t next = 0;
        std::uint64_t acceptedSwap = 0;
        std::array<std::uint64_t, maxLanes> generation;
        generation.fill(1);
        std::uint64_t gameUpdate = 0;
        es_rt_token nextEngine{};
        while (!startAt.load(std::memory_order_acquire) && !done.load(std::memory_order_acquire) &&
                !stop.load(std::memory_order_acquire)) std::this_thread::yield();
        const auto start = startAt.load(std::memory_order_acquire);
        while (!done.load(std::memory_order_acquire) && !stop.load(std::memory_order_acquire)) {
            checkAcks();
            const auto transport = position();
            if (transport.broken) {
                stop.store(true, std::memory_order_release);
                break;
            }
            const auto lead = transport.generated - transport.submitted;
            metrics.lowLead = std::min(metrics.lowLead, lead);
            metrics.highLead = std::max(metrics.highLead, lead);
            metrics.queueRejects = transport.queue_rejections;
            metrics.droppedSnapshots = transport.dropped_snapshots;
            if (settings.live && gameUpdate * settings.rate < settings.frames * 60) {
                const auto due = start + gameUpdate * UINT64_C(1000000000) / 60;
                const auto observed = ns();
                if (observed >= due) {
                    updates.push_back({due, observed, transport.submitted, transport.generated, transport.sealed});
                    ++gameUpdate;
                }
            }
            if (next < agenda.size()) {
                const auto &action = agenda[next];
                const auto current = ns();
                const auto relative = (action.kind == Event::release && acceptedSwap) ?
                    acceptedSwap + (settings.rate + 2) / 3 : action.fire;
                const auto trigger = start + relative * UINT64_C(1000000000) / settings.rate;
                const bool ready = action.kind == Event::prepare && settings.pace ? current >= trigger :
                    (settings.live ? current >= trigger : transport.submitted + settings.rate / 10 >= relative);
                if (ready) {
                    ++next;
                    if (action.kind == Event::prepare) {
                        nextEngine = candidate(action.lane, action.seed);
                        continue;
                    }
                    const auto d = position().submitted;
                    const auto sampled = ns();
                    const auto target = settings.live ? d + high + q : relative;
                    if (action.kind == Event::activate) {
                        auto event = command(ES_RT_ACTIVATE, target);
                        event.next_voice = {1, action.lane, 0};
                        event.duration_ms = ES_RT_DFLT_MS;
                        event.candidate = candidate(action.lane, action.seed);
                        (void)send(event, trigger, 0, d);
                    } else if (action.kind == Event::swap) {
                        auto event = command(ES_RT_REPLACE, target);
                        event.voice = {generation[action.lane], action.lane, 0};
                        event.next_voice = {generation[action.lane] + 1, action.lane, 0};
                        event.candidate = nextEngine;
                        event.duration_ms = ES_RT_DFLT_MS;
                        if (send(event, trigger, settings.live ? sampled : 0, d)) {
                            acceptedSwap = target;
                            ++generation[action.lane];
                            nextEngine = {};
                        }
                    } else {
                        auto event = command(action.kind == Event::gain ? ES_RT_GAIN : ES_RT_VECTOR, target);
                        event.voice = {generation[action.lane], action.lane, 0};
                        event.gain = action.gainTarget;
                        event.physical = action.controls;
                        (void)send(event, trigger, settings.live ? sampled : 0, d);
                    }
                    continue;
                }
            }
            generated.store(transport.generated, std::memory_order_release);
            if (settings.pace) std::this_thread::sleep_for(100us);
            else std::this_thread::yield();
        }
        if (nextEngine.epoch) (void)es_rt_discard(fleet.get(), nextEngine);
        metrics.unsubmitted = agenda.size() - next;
    }

    void pace(Clock::time_point deadline, std::uint64_t span) const {
        const auto halfPeriod = Ns(span * UINT64_C(500000000) / settings.rate);
        const auto early = deadline - std::min(Ns(paceSpin), halfPeriod);
        if (Clock::now() < early) std::this_thread::sleep_until(early);
        while (Clock::now() < deadline) {}
    }

#if defined(ENGINE_SIM_FLEET_BENCH_PORTAUDIO)
    static int deviceEntry(const void *, void *output, unsigned long frames,
                           const PaStreamCallbackTimeInfo *timing, PaStreamCallbackFlags flags, void *context) noexcept {
        return static_cast<Trial *>(context)->deviceBlock(static_cast<float *>(output), frames, timing, flags);
    }

    void runDevice() {
        PortAudio provider;
        const auto *info = Pa_GetDeviceInfo(settings.device);
        require(info && info->maxOutputChannels > 0, "Unknown physical output");
        const auto *host = Pa_GetHostApiInfo(info->hostApi);
        const std::string_view name(info->name);
        require(host && host->type == paALSA && name == settings.deviceName &&
                name.find("(hw:") != std::string_view::npos,
                "Physical device index must match the explicitly named native ALSA endpoint");
        PaStreamParameters output{settings.device, 1, paFloat32, info->defaultLowOutputLatency, nullptr};
        checkPa(Pa_IsFormatSupported(nullptr, &output, settings.rate), "Physical output format");
        PaStream *handle = nullptr;
        checkPa(Pa_OpenStream(&handle, nullptr, &output, settings.rate, settings.block, paNoFlag,
                             deviceEntry, this), "Opening physical endpoint");
        const auto close = [](PaStream *stream) { (void)Pa_CloseStream(stream); };
        std::unique_ptr<PaStream, decltype(close)> stream(handle, close);
        const auto *actual = Pa_GetStreamInfo(handle);
        require(actual && actual->sampleRate == settings.rate && std::isfinite(actual->outputLatency) &&
                actual->outputLatency > 0, "Physical output negotiated a different rate or invalid latency");
        const auto before = ns();
        const auto backend = Pa_GetStreamTime(handle);
        const auto after = ns();
        require(std::isfinite(backend) && backend > 0 && (after - before) * settings.rate < UINT64_C(1000000000),
                "Cannot calibrate monotonic and PortAudio clocks");
        deviceUncertainty = (after - before) * .5;
        deviceOffset = before + deviceUncertainty - backend * 1e9;
        std::cout << std::setprecision(17)
                  << "provider=" << Pa_GetVersionText() << " endpoint=" << info->name << " backend=" << host->name
                  << " rate=" << actual->sampleRate << " requested_period=" << settings.block
                  << " output_latency_s=" << actual->outputLatency
                  << " clock_before_ns=" << before << " stream_s=" << backend << " clock_after_ns=" << after
                  << " offset_ns=" << deviceOffset << " uncertainty_ns=" << deviceUncertainty << std::endl;
        cpu_set_t previous;
        require(pthread_getaffinity_np(pthread_self(), sizeof(previous), &previous) == 0, "Read coordinator affinity");
        bind(settings.audioCore);
        if (settings.live) metrics.cpuIterations = calibrateAudio(deviceWork);
        const auto started = Pa_StartStream(handle);
        require(pthread_setaffinity_np(pthread_self(), sizeof(previous), &previous) == 0, "Restore coordinator affinity");
        checkPa(started, "Start native callback");
        coordinate();
        if (done.load(std::memory_order_acquire) && !stop.load(std::memory_order_acquire)) {
            const auto deadline = Clock::now() + 1s;
            // PortAudio must finish draining the last successful device buffer.
            while (Pa_IsStreamActive(handle) == 1 && Clock::now() < deadline)
                std::this_thread::sleep_for(100us);
            require(Pa_IsStreamActive(handle) == 0, "Physical endpoint did not drain");
        } else {
            (void)Pa_AbortStream(handle);
        }
        const auto endBefore = ns();
        const auto endBackend = Pa_GetStreamTime(handle);
        const auto endAfter = ns();
        std::cout << std::setprecision(17) << "clock_end_before_ns=" << endBefore << " stream_end_s=" << endBackend
                  << " clock_end_after_ns=" << endAfter << std::endl;
        stream.reset();
    }
#endif

    std::uint64_t calibrateAudio(volatile std::uint64_t &work) const noexcept {
        if (!settings.live) return 0;
        constexpr std::uint64_t iterations = 256000;
        std::array<std::uint64_t, 9> samples{};
        for (auto &sample : samples) {
            const auto before = ns();
            work ^= kernel(before, iterations);
            sample = ns() - before;
        }
        std::sort(samples.begin(), samples.end());
        const auto elapsed = std::max<std::uint64_t>(samples[samples.size() / 2], 1);
        return std::clamp<std::uint64_t>(iterations * settings.block * UINT64_C(100000000) /
            settings.rate / elapsed, 1, 5'000'000);
    }

#if defined(ENGINE_SIM_FLEET_BENCH_PORTAUDIO)
    int deviceBlock(float *deviceOutput, unsigned long frames,
                    const PaStreamCallbackTimeInfo *timing, PaStreamCallbackFlags flags) noexcept {
        const auto arrived = ns();
        const auto first = ended;
        if (stop.load(std::memory_order_acquire)) return paAbort;
        const auto reject = [&]() noexcept {
            ++metrics.breaks;
            done.store(true, std::memory_order_release);
            return paAbort;
        };
        if (!deviceOutput || !timing || records.size() >= records.capacity() ||
                deviceRecords.size() >= deviceRecords.capacity()) return reject();
        deviceRecords.push_back({first, frames, flags, timing->inputBufferAdcTime, timing->currentTime, timing->outputBufferDacTime});
        if (frames != settings.block || !std::isfinite(timing->currentTime) ||
            !std::isfinite(timing->outputBufferDacTime) || timing->outputBufferDacTime <= timing->currentTime) return reject();
        if (!first) {
            metrics.audioRounding = std::fegetround();
            callbackScheduler = sched_getscheduler(0);
            sched_param actual{};
            if (sched_getparam(0, &actual) == 0) callbackPriority = actual.sched_priority;
            callbackCpu = sched_getcpu();
            startAt.store(arrived, std::memory_order_release);
        }
        auto out = init<es_rt_audio>();
        out.frames = settings.block;
        out.capacity = settings.block;
        out.mix = deviceOutput;
        for (unsigned lane = 0; lane < settings.roster.size(); ++lane) out.stem[lane] = deviceStem[lane].data();
        auto reply = init<es_rt_block>();
        const auto entered = ns();
        const auto status = es_rt_process(fleet.get(), &out, &reply);
        const auto returned = ns();
        if (metrics.cpuIterations) deviceWork = kernel(deviceWork ^ first, metrics.cpuIterations);
        if (status == ES_RT_OK && reply.first == first && reply.epoch == epoch && reply.pcm_frames == frames)
            accumulate(first, out.frames, deviceOutput, deviceStem);
        const auto finished = ns();
        const auto deadline = static_cast<std::uint64_t>(timing->outputBufferDacTime * 1e9 + deviceOffset - deviceUncertainty);
        records.push_back({first, arrived, entered, returned, finished, reply.pcm_frames, reply.fallback_frames, status});
        if (finished > deadline) ++metrics.deadlineMisses;
        metrics.fallback += reply.fallback_frames;
        if (status != ES_RT_OK || reply.first != first || reply.epoch != epoch || reply.pcm_frames != frames) return reject();
        if (flags & xruns) ++deviceXruns;
        ended += frames;
        if (ended != settings.frames && !metrics.deadlineMisses && !deviceXruns) return paContinue;
        metrics.elapsed = finished - startAt.load(std::memory_order_relaxed);
        metrics.audioFinalRounding = std::fegetround();
        audioDigest = deviceWork;
        rusage usage{};
        if (getrusage(RUSAGE_THREAD, &usage) == 0)
            metrics.audioCpuNs = (static_cast<std::uint64_t>(usage.ru_utime.tv_sec) + usage.ru_stime.tv_sec) *
                UINT64_C(1000000000) + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * UINT64_C(1000);
        done.store(true, std::memory_order_release);
        return paComplete;
    }
#endif

    void pump() {
        bind(settings.audioCore);
        metrics.audioRounding = std::fegetround();
        volatile std::uint64_t audioWork = 1;
        const auto calibration = calibrateAudio(audioWork);
        metrics.cpuIterations = calibration;
        std::array<float, maximum> mix{};
        std::array<std::array<float, maximum>, maxLanes> stem{};
        auto out = init<es_rt_audio>();
        out.capacity = settings.block;
        out.mix = mix.data();
        for (unsigned lane = 0; lane < settings.roster.size(); ++lane) out.stem[lane] = stem[lane].data();
        const auto start = Clock::now() + 10ms;
        startAt.store(ns(start), std::memory_order_release);
        pace(start, settings.block);
        const std::array<unsigned, 3> period{settings.block, settings.block / 2, 3 * settings.block / 4};
        for (std::uint64_t first = 0, call = 0; first < settings.frames && !stop.load(std::memory_order_acquire); ++call) {
            const auto nominal = period[settings.variable ? call % period.size() : 0];
            const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(nominal, settings.frames - first));
            out.frames = count;
            auto reply = init<es_rt_block>();
            const auto due = start + Ns(first * UINT64_C(1000000000) / settings.rate);
            const auto end = start + Ns((first + count) * UINT64_C(1000000000) / settings.rate);
            if (settings.pace && !settings.injectStarve) pace(due, count);
            if (!settings.pace) {
                while (generated.load(std::memory_order_acquire) < first + count) {
                    if (stop.load(std::memory_order_acquire)) break;
                    std::this_thread::yield();
                }
            }
            if (settings.injectMiss && first == 0) std::this_thread::sleep_until(end + 1ms);
            const auto entered = ns();
            const auto status = es_rt_process(fleet.get(), &out, &reply);
            const auto returned = ns();
            if (calibration) audioWork = kernel(audioWork ^ first, calibration);
            const auto finished = ns();
            records.push_back({first, ns(due), entered, returned, finished, reply.pcm_frames,
                               reply.fallback_frames, status});
            if (settings.pace && finished > ns(end)) ++metrics.deadlineMisses;
            metrics.fallback += reply.fallback_frames;
            if (status != ES_RT_OK || reply.first != first || reply.epoch != epoch || reply.pcm_frames != count) {
                ++metrics.breaks;
                break;
            }
            accumulate(first, count, mix.data(), stem);
            first += count;
            ended = first;
            if (metrics.deadlineMisses) break;
        }
        if (settings.pace && ended == settings.frames)
            pace(start + Ns(settings.frames * UINT64_C(1000000000) / settings.rate), settings.block);
        metrics.elapsed = ns() - ns(start);
        audioDigest = audioWork;
        metrics.audioFinalRounding = std::fegetround();
#if defined(__linux__)
        rusage usage{};
        if (getrusage(RUSAGE_THREAD, &usage) == 0)
            metrics.audioCpuNs = (static_cast<std::uint64_t>(usage.ru_utime.tv_sec) + usage.ru_stime.tv_sec) *
                UINT64_C(1000000000) + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * UINT64_C(1000);
#endif
        done.store(true, std::memory_order_release);
    }
    void accumulate(std::uint64_t first, unsigned count, const float *mix,
                    const std::array<std::array<float, maximum>, maxLanes> &stem) noexcept {
        for (unsigned i = 0; i < count; ++i) {
            auto &window = windows[static_cast<std::size_t>((first + i) / settings.rate)];
            ++window.frames;
            if (!std::isfinite(mix[i])) ++metrics.invalidSamples;
            metrics.mixPeak = std::max(metrics.mixPeak, std::abs(mix[i]));
            if (std::abs(mix[i]) == 1.f) ++metrics.clipped;
            for (unsigned lane = 0; lane < settings.roster.size(); ++lane) {
                const auto sample = stem[lane][i];
                if (!std::isfinite(sample)) ++metrics.invalidSamples;
                if (sample != 0.f) ++window.nonzero[lane];
                std::uint32_t raw;
                std::memcpy(&raw, &sample, sizeof(raw));
                metrics.hash[lane] = (metrics.hash[lane] ^ raw) * digestPrime;
                window.hash[lane] = (window.hash[lane] ^ raw) * digestPrime;
            }
        }
    }

    void capture() {
        std::ofstream raw(settings.path / "callbacks.bin", std::ios::binary);
        raw.write(reinterpret_cast<const char *>(records.data()), static_cast<std::streamsize>(records.size() * sizeof(ClockSample)));
#if defined(ENGINE_SIM_FLEET_BENCH_PORTAUDIO)
        if (settings.device >= 0) {
            std::ofstream timestamps(settings.path / "device.bin", std::ios::binary);
            timestamps.write(reinterpret_cast<const char *>(deviceRecords.data()),
                             static_cast<std::streamsize>(deviceRecords.size() * sizeof(DeviceSample)));
            std::ofstream summary(settings.path / "device.tsv");
            summary << std::setprecision(17) << "offset_ns " << deviceOffset << '\n'
                    << "uncertainty_ns " << deviceUncertainty << '\n'
                    << "xruns " << deviceXruns << '\n' << "audio_cpu " << callbackCpu << '\n'
                    << "audio_scheduler " << callbackScheduler << '\n' << "audio_priority " << callbackPriority << '\n';
            require(timestamps && summary, "Write native physical timestamp evidence");
        }
#endif

        std::ofstream actions(settings.path / "events.tsv");
        actions << "sequence\tkind\tframe\ttrigger\tsampled\tD\tsubmit\treason\tapplied\tended\treaped\tlane\tvoice\tnext"
                   "\tdemand\tload\trpm\tignition\tstarter\tgain\n";
        for (const auto &event : events) {
            const auto &c = event.event;
            actions << c.sequence << '\t' << c.kind << '\t' << c.frame << '\t' << event.trigger << '\t'
                    << event.sampled << '\t' << event.submitted << '\t' << event.issue << '\t' << event.result << '\t'
                    << event.applied << '\t' << event.end << '\t' << event.receiptAt << '\t'
                    << (c.next_voice.generation ? c.next_voice.lane : c.voice.lane) << '\t' << c.voice.generation << '\t'
                    << c.next_voice.generation << '\t' << std::hexfloat << c.physical.demand << '\t'
                    << c.physical.load_nm << '\t' << c.physical.driven_rpm << std::defaultfloat << '\t' << c.physical.ignition
                    << '\t' << c.physical.starter << '\t' << std::hexfloat << c.gain << std::defaultfloat << '\n';
        }
        std::ofstream progress(settings.path / "windows.tsv");
        progress << "frames";
        for (unsigned lane = 0; lane < settings.roster.size(); ++lane) progress << "\tnonzero" << lane;
        progress << '\n';
        for (const auto &window : windows) {
            progress << window.frames;
            for (unsigned lane = 0; lane < settings.roster.size(); ++lane) progress << '\t' << window.nonzero[lane];
            progress << '\n';
        }
        std::ofstream digests(settings.path / "digests.tsv");
        digests << "lane\twindow\tdigest\n";
        for (unsigned lane = 0; lane < settings.roster.size(); ++lane) {
            for (std::size_t i = 0; i < windows.size() && windows[i].frames != 0; ++i)
                digests << lane << '\t' << i << '\t' << windows[i].hash[lane] << '\n';
            digests << lane << "\tall\t" << metrics.hash[lane] << '\n';
        }
        std::ofstream info(settings.path / "counts.tsv");
        std::uint32_t peakBits;
        std::memcpy(&peakBits, &metrics.mixPeak, sizeof(peakBits));
        info << "elapsed_ns " << metrics.elapsed << "\ndeadline_misses " << metrics.deadlineMisses
             << "\ntransport_breaks " << metrics.breaks << "\nfallback_frames " << metrics.fallback
             << "\nlate_events " << metrics.late << "\nunsubmitted_events " << metrics.unsubmitted
             << "\nunexpected_faults " << metrics.faults
             << "\noverlap_completions " << metrics.overlaps << "\nmax_lookahead_frames " << metrics.lookahead
             << "\ninvalid_samples " << metrics.invalidSamples << "\nclipped " << metrics.clipped
             << "\nmix_peak_float_bits " << peakBits
             << "\nprep_max_ns " << metrics.prepMax << "\nstartup_ns " << metrics.startupNs
             << "\nconsumer_rounding_before " << metrics.audioRounding << "\nconsumer_rounding_after " << metrics.audioFinalRounding
             << "\nconsumer_cpu_ns " << metrics.audioCpuNs << "\nmin_sampled_lead_frames " << metrics.lowLead
             << "\nmax_sampled_lead_frames " << metrics.highLead << "\nqueue_rejections " << metrics.queueRejects
             << "\nsnapshot_drops " << metrics.droppedSnapshots
             << "\nhost_load_iterations " << metrics.cpuIterations << "\naudio_work_digest " << audioDigest
             << "\nhost_pace_spin_us " << paceSpin.count()
             << "\nbackground_work_digest " << loadDigest << "\nframes " << ended << '\n';
        for (unsigned lane = 0; lane < settings.roster.size(); ++lane)
            info << "ticks" << lane << ' ' << metrics.ticks[lane] << "\nstem" << lane << ' ' << metrics.hash[lane] << '\n';
        std::ofstream returns(settings.path / "retired.tsv");
        returns << "lane\tgeneration\tframes\tticks\tnative\tfault\n";
        for (const auto &state : retired) returns << state.voice.lane << '\t' << state.voice.generation << '\t'
            << state.frames << '\t' << state.ticks << '\t' << state.native_frames << '\t' << state.fault << '\n';
        std::ofstream game(settings.path / "game.tsv");
        game << "due\tobserved\tD\tG\tS\n";
        for (const auto &update : updates) game << update.due << '\t' << update.observed << '\t' << update.submitted
            << '\t' << update.generated << '\t' << update.sealed << '\n';
        std::ofstream trace(settings.path / "voices.tsv");
        trace << "G\tlane\tgeneration\tframes\tticks\tnative\tfault\trpm\tphase\tdemand\tload\ttarget_rpm\n";
        for (const auto &point : voicePoints) {
            const auto &v = point.info;
            trace << point.generated << '\t' << v.voice.lane << '\t' << v.voice.generation << '\t' << v.frames << '\t'
                  << v.ticks << '\t' << v.native_frames << '\t' << v.fault << '\t' << std::hexfloat << v.measured_rpm
                  << '\t' << v.cycle_rad << '\t' << v.applied.demand << '\t' << v.applied.load_nm << '\t'
                  << v.applied.driven_rpm << std::defaultfloat << '\n';
        }
#if defined(__linux__)
        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) == 0) info << "process_peak_rss_kib " << usage.ru_maxrss << '\n';
#endif
        require(raw && actions && progress && digests && info && returns && game && trace, "write native benchmark evidence");
    }

    const Settings settings;
    const std::chrono::microseconds paceSpin{1000};
    Owner fleet{nullptr, es_rt_destroy};
    const std::vector<Event> agenda;
    std::vector<Window> windows;
    std::vector<ClockSample> records;
#if defined(ENGINE_SIM_FLEET_BENCH_PORTAUDIO)
    std::vector<DeviceSample> deviceRecords;
    std::array<std::array<float, maximum>, maxLanes> deviceStem{};
    std::uint64_t deviceWork = 1;
    int callbackCpu = -1, callbackScheduler = -1, callbackPriority = -1;
    double deviceOffset = 0., deviceUncertainty = 0.;
#endif

    std::vector<LoggedEvent> events;
    std::vector<GameUpdate> updates;
    std::vector<VoicePoint> voicePoints;
    std::vector<es_rt_lane_info> retired;
    std::deque<std::size_t> outstanding;
    std::array<bool, maxLanes> seenFault{};
    std::array<std::uint64_t, maxLanes> lastGeneration{}, lastSecond{};
    Metrics metrics;
    std::uint64_t ended = 0, audioDigest = 0, loadDigest = 0, deviceXruns = 0;
    std::uint32_t high = 0;
    std::atomic<std::uint64_t> startAt{0}, generated{0};
    std::atomic<bool> done{false}, stop{false};
};

}

int main(int argc, char **argv) {
    if (argc != 14) return 2;
    try {
        const std::string_view workload = argv[10], variant = argv[13];
        auto settings = Settings{argv[1], argv[2], argv[3], argv[4], argv[5],
            number<unsigned>(argv[6]), number<unsigned>(argv[7]), number<unsigned>(argv[8]),
            number<std::uint64_t>(argv[9]), number<int>(argv[11]), number<int>(argv[12]),
            workload == "live-loaded", variant != "unp" && variant != "replay-smoke", variant == "variable",
            variant == "miss", variant == "starve", variant == "replay-smoke"};
        if (const auto *roster = std::getenv("ES_FLEET_BENCH_ROSTER")) settings.roster = roster;
        if (const auto *ls = std::getenv("ES_FLEET_BENCH_LS_MODEL")) settings.ls = ls;
        require(!settings.roster.empty() && settings.roster.size() <= maxLanes &&
                settings.roster.find_first_not_of("FDL") == std::string::npos &&
                (settings.roster.find('L') == std::string::npos || !settings.ls.empty()) &&
                (!(settings.live || settings.replaySmoke) || settings.roster == "FD"), "invalid benchmark roster");
        require(settings.block && settings.block <= maximum && (settings.rate == 44100 || settings.rate == 48000) &&
                (settings.profile == ES_RT_BALANCED || settings.profile == ES_RT_INTERACTIVE) &&
                settings.frames && settings.frames <= settings.rate * UINT64_C(600), "Invalid benchmark frame span");
        require((workload == "scheduled" || workload == "live-loaded") &&
                (variant == "normal" || variant == "unp" || variant == "miss" || variant == "starve" || variant == "variable" ||
                 settings.replaySmoke) &&
                ((!settings.variable && settings.pace) || !settings.live), "Invalid benchmark workload");
        if (const auto *device = std::getenv("ES_FLEET_BENCH_DEVICE")) {
#if defined(ENGINE_SIM_FLEET_BENCH_PORTAUDIO)
            settings.device = number<int>(device);
            settings.deviceName = std::getenv("ES_FLEET_BENCH_DEVICE_NAME");
            require(settings.deviceName && *settings.deviceName, "Supply the exact ALSA device name");
            require(settings.device >= 0 && variant == "normal" && settings.roster == "FD" &&
                    settings.frames % settings.block == 0 && settings.rate == 48000 &&
                    ((settings.profile == ES_RT_INTERACTIVE && settings.block == 128) ||
                     (settings.profile == ES_RT_BALANCED && settings.block == 256)), "Invalid physical trial tuple");
#else
            (void)device;
            throw std::runtime_error("Build the installed benchmark with ENGINE_SIM_FLEET_BENCH_PORTAUDIO");
#endif
        }
        Trial trial(settings);
        return trial.run() ? 0 : 1;
    } catch (const std::exception &issue) {
        std::cerr << issue.what() << '\n';
        return 1;
    }
}
