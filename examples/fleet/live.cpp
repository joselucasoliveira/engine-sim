#include "adapter.hpp"
#include <portaudio.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
using namespace fleet_example;
using namespace std::chrono_literals;
constexpr std::uint64_t playbackFrames = 68000;

template<class Record>
Record record() {
    Record out{};
    out.size = sizeof(out);
    out.abi = ES_RT_V1;
    return out;
}

void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void checkPa(PaError code, const char *operation) {
    if (code != paNoError) throw std::runtime_error(std::string(operation) + " " + Pa_GetErrorText(code));
}
struct PortAudio {
    PortAudio() { checkPa(Pa_Initialize(), "PortAudio initialization"); }
    ~PortAudio() { (void)Pa_Terminate(); }
};

std::string libraryPath() {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&Pa_GetVersionInfo), &module)) return {};
    std::array<char, 4096> path{};
    const auto resolved = GetModuleFileNameA(module, path.data(), static_cast<DWORD>(path.size()));
    return resolved && resolved < path.size() ? std::string(path.data(), resolved) : std::string{};
#else
    Dl_info info{};
    return dladdr(reinterpret_cast<const void *>(&Pa_GetVersionInfo), &info) && info.dli_fname ? info.dli_fname : "";
#endif
}

int callback(const void *, void *deviceOutput, unsigned long frames,
             const PaStreamCallbackTimeInfo *timing, PaStreamCallbackFlags flags, void *data) noexcept {
    auto &adapter = *static_cast<Adapter *>(data);
    StreamTimes observed{};
    if (timing) observed = {timing->inputBufferAdcTime, timing->currentTime, timing->outputBufferDacTime};
    const auto result = adapter.process(static_cast<float *>(deviceOutput), frames, timing ? &observed : nullptr, flags);
    if (result == CallbackReply::abort) return paAbort;
    return result == CallbackReply::complete ? paComplete : paContinue;
}

struct Device {
    PaStreamParameters output{};
    unsigned rate;
    unsigned period;

    Device(PaDeviceIndex endpoint, unsigned fs, unsigned maximum) : rate(fs), period(maximum) {
        const auto *info = Pa_GetDeviceInfo(endpoint);
        check(info && info->maxOutputChannels > 0, "Select a valid output endpoint index");
        output.device = endpoint;
        output.channelCount = 1;
        output.sampleFormat = paFloat32;
        output.suggestedLatency = info->defaultLowOutputLatency;
        if (Pa_IsFormatSupported(nullptr, &output, rate) != paFormatIsSupported) {
            check(info->maxOutputChannels >= 2, "Endpoint cannot open the requested float format");
            output.channelCount = 2;
            checkPa(Pa_IsFormatSupported(nullptr, &output, rate), "Negotiating stereo output");
        }
        const auto *backend = Pa_GetHostApiInfo(info->hostApi);
        check(backend, "Host API unavailable");
        std::cout << "endpoint=" << endpoint << " endpoint_name=" << info->name << " backend=" << backend->name
                  << " rate=" << rate << " period=" << period << " channels=" << output.channelCount << '\n';
    }
};

es_rt_event event(std::uint64_t epoch, unsigned sequence, unsigned frame, unsigned kind,
                  std::uint32_t lane) {
    auto event = record<es_rt_event>();
    event.epoch = epoch;
    event.sequence = sequence;
    event.frame = frame;
    event.kind = kind;
    if (kind == ES_RT_ACTIVATE) event.next_voice = {1, lane, 0};
    else event.voice = {1, lane, 0};
    return event;
}

void submit(es_rt_fleet *fleet, const es_rt_event &value) {
    check(es_rt_submit(fleet, &value) == ES_RT_OK, "Initial Fleet command rejected");
}

void prepare(es_rt_fleet *fleet, std::uint64_t epoch, const std::array<const char *, 4> &files) {
    auto diagnostic = record<es_rt_diagnostic>();
    for (std::uint32_t lane = 0; lane < 2; ++lane) {
        auto config = record<es_rt_voice_config>();
        config.mode = lane ? ES_RT_DRIVEN : ES_RT_FREE;
        config.seed = 71 + lane;
        config.controls_policy = ES_RT_GAME;
        config.initial = {.25, 0., 0., 1, lane ? 0u : 1u};
        auto activate = event(epoch, lane + 1, 0, ES_RT_ACTIVATE, lane);
        activate.duration_ms = ES_RT_DFLT_MS;
        check(es_rt_prepare_file(fleet, &config, files[lane * 2], files[lane * 2 + 1],
                                  &activate.candidate, &diagnostic) == ES_RT_OK, diagnostic.message);
        submit(fleet, activate);
    }
    auto spin = event(epoch, 3, 4800, ES_RT_VECTOR, 1);
    spin.physical = {.3, 0., 600., 1, 0};
    submit(fleet, spin);
    auto release = event(epoch, 4, 16000, ES_RT_VECTOR, 0);
    release.physical = {.55, 0., 0., 1, 0};
    submit(fleet, release);
    auto load = event(epoch, 5, 25200, ES_RT_VECTOR, 1);
    load.physical = {.65, 2., 900., 1, 0};
    submit(fleet, load);
    auto stop = event(epoch, 6, 60000, ES_RT_STOP, 0);
    stop.duration_ms = 5;
    submit(fleet, stop);
    stop = event(epoch, 7, 66000, ES_RT_STOP, 1);
    stop.duration_ms = 5;
    submit(fleet, stop);
    check(es_rt_start(fleet, &diagnostic) == ES_RT_OK, diagnostic.message);
}

double monotonic() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Counters {
    std::uint64_t frames = 0, fallback = 0, callbacks = 0, xruns = 0;
    std::uint64_t minimumCredit = UINT64_MAX, maximumCredit = 0;
    std::size_t minimum = Adapter::deviceLimit, maximum = 0;
    std::array<std::uint64_t, 2> signal{}, ticks{};
    double largestResidual = 0;
    bool broken = false;
};

bool play(const std::array<const char *, 4> &files, const Device &device, std::uint64_t epoch) {
    auto configuration = record<es_rt_config>();
    configuration.rate = device.rate;
    configuration.max_frames = device.period;
    configuration.profile = ES_RT_BALANCED;
    configuration.lanes = 2;
    configuration.epoch = epoch;
    auto diagnostic = record<es_rt_diagnostic>();
    es_rt_fleet *fleet = nullptr;
    check(es_rt_create(&configuration, &fleet, &diagnostic) == ES_RT_OK, diagnostic.message);
    const std::unique_ptr<es_rt_fleet, decltype(&es_rt_destroy)> host(fleet, es_rt_destroy);
    constexpr auto xruns = paInputUnderflow | paInputOverflow | paOutputUnderflow | paOutputOverflow;
    Adapter adapter(fleet, device.period, static_cast<unsigned>(device.output.channelCount), xruns, playbackFrames);
    PaStream *handle = nullptr;
    checkPa(Pa_OpenStream(&handle, nullptr, &device.output, device.rate, device.period, paNoFlag,
                         callback, &adapter), "Opening endpoint");
    const auto close = [](PaStream *stream) { (void)Pa_CloseStream(stream); };
    std::unique_ptr<PaStream, decltype(close)> stream(handle, close);
    const auto *actual = Pa_GetStreamInfo(handle);
    check(actual && actual->sampleRate == device.rate && std::isfinite(actual->outputLatency) &&
          actual->outputLatency >= 0., "Unexpected device sample rate or latency");
    std::cout << std::setprecision(17) << "reported_output_latency_s=" << actual->outputLatency
              << " actual_rate=" << actual->sampleRate << " epoch=" << epoch << '\n';
    prepare(fleet, epoch, files);
    Presentation timeline(device.rate);
    const auto before = monotonic();
    const auto streamTime = Pa_GetStreamTime(handle);
    const auto after = monotonic();
    check(timeline.calibrate(before, streamTime, after), "Invalid stream-clock calibration");
    std::cout << "clock_sample_monotonic_before=" << before << " stream=" << streamTime
              << " monotonic_after=" << after << " uncertainty_s=" << timeline.uncertainty << '\n';
    checkPa(Pa_StartStream(handle), "Starting endpoint");

    Counters counts;
    const auto drain = [&] {
        CallbackRecord note{};
        for (unsigned i = 0; i < 256 && adapter.poll(note); ++i) {
            ++counts.callbacks;
            counts.minimum = std::min(counts.minimum, note.frames);
            counts.maximum = std::max(counts.maximum, note.frames);
            counts.fallback += note.fallback;
            if (note.first != ES_RT_NO_FRAME) counts.frames += note.pcm + note.fallback;
            if (note.flags & xruns) ++counts.xruns;
            for (unsigned lane = 0; lane < 2; ++lane) counts.signal[lane] += note.nonzero[lane];
            if (!timeline.observe(note) || note.frames != device.period) {
                counts.broken = true;
                adapter.requestStop();
            } else {
                counts.largestResidual = std::max(counts.largestResidual, std::abs(timeline.frameResidual));
            }
        }
        auto snapshot = record<es_rt_snapshot>();
        for (unsigned i = 0; i < 16 && es_rt_poll_snapshot(fleet, &snapshot) == ES_RT_OK; ++i) {
            for (unsigned lane = 0; lane < 2; ++lane) {
                counts.ticks[lane] = std::max(counts.ticks[lane], snapshot.lane[lane].ticks);
                if (snapshot.lane[lane].fault) counts.broken = true;
            }
        }
        auto transport = record<es_rt_transport>();
        if (es_rt_get_transport(fleet, &transport) != ES_RT_OK || transport.broken) counts.broken = true;
        const auto lead = transport.generated > transport.submitted ? transport.generated - transport.submitted : 0;
        counts.minimumCredit = std::min(counts.minimumCredit, lead);
        counts.maximumCredit = std::max(counts.maximumCredit, lead);
    };
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        drain();
        const auto active = Pa_IsStreamActive(handle);
        if (active < 0 || std::chrono::steady_clock::now() >= deadline) counts.broken = true;
        if (counts.broken) {
            adapter.requestStop();
            if (active != 0) (void)Pa_AbortStream(handle);
            break;
        }
        if (!active) break;
        std::this_thread::sleep_for(1ms);
    }
    // Closing the backend provides the callback barrier. All Fleet ownership
    // changes below occur on this coordinator.
    stream.reset();
    drain();
    check(es_rt_quiesce(fleet) == ES_RT_OK, "Fleet quiesce");
    unsigned receipts = 0, returned = 0;
    for (std::uint64_t sequence = 1; sequence <= 7; ++sequence) {
        auto result = record<es_rt_result>();
        if (es_rt_poll_result(fleet, sequence, &result) == ES_RT_OK && result.reason == ES_RT_OK) ++receipts;
    }
    es_rt_token token{};
    es_rt_lane_info state{};
    while (es_rt_collect(fleet, &token, &state) == ES_RT_OK) ++returned;
    std::cout << "callbacks=" << counts.callbacks << " min_period=" << counts.minimum
              << " max_period=" << counts.maximum << " submitted=" << counts.frames
              << " xruns=" << counts.xruns << " fallback=" << counts.fallback << " dropped_timestamps=" << adapter.dropped()
              << " lead_min=" << counts.minimumCredit << " lead_max=" << counts.maximumCredit
              << " dac_estimate_host_seconds=" << timeline.hostDacEstimate()
              << " max_dac_residual_frames=" << counts.largestResidual << '\n';
    std::cout << "lane0_nonzero=" << counts.signal[0] << " lane1_nonzero=" << counts.signal[1]
              << " lane0_ticks=" << counts.ticks[0] << " lane1_ticks=" << counts.ticks[1]
              << " receipts=" << receipts << " returned=" << returned << '\n';
    return !counts.broken && counts.frames >= playbackFrames && counts.minimum == device.period &&
           counts.maximum == device.period && !counts.xruns && !counts.fallback && !adapter.dropped() &&
           counts.signal[0] && counts.signal[1] && counts.ticks[0] && counts.ticks[1] && receipts == 7 && returned == 2;
}
}

int main(int argc, char **argv) {
    if (argc != 8 && argc != 9) {
        std::cerr << "usage: engine-sim-fleet-live ENDPOINT RATE PERIOD ABS_MODEL_A ABS_ROOT_A ABS_MODEL_B ABS_ROOT_B [--recover-once]\n";
        return EXIT_FAILURE;
    }
    try {
        const int endpoint = std::stoi(argv[1]);
        const auto rate = static_cast<unsigned>(std::stoul(argv[2]));
        const auto period = static_cast<unsigned>(std::stoul(argv[3]));
        const bool recover = argc == 9 && std::string(argv[8]) == "--recover-once";
        check((argc == 8 || recover) && (rate == 44100 || rate == 48000) &&
              (period == 64 || period == 128 || period == 256 || period == 512 || period == 1024),
              "Unsupported rate, period or recovery option");
        PortAudio audio;
        const auto *version = Pa_GetVersionInfo();
        check(version && version->versionMajor == 19, "PortAudio major version 19 is required");
        const auto path = libraryPath();
        check(!path.empty(), "Cannot resolve the loaded PortAudio library");
        std::cout << "portaudio_version=" << version->versionText << "\nportaudio_library=" << path << '\n';
        const std::array<const char *, 4> files{argv[4], argv[5], argv[6], argv[7]};
        if (play(files, Device(endpoint, rate, period), 1)) return EXIT_SUCCESS;
        if (recover && play(files, Device(endpoint, rate, period), 2)) return EXIT_SUCCESS;
        std::cerr << "Endpoint run ended with a discontinuity or an incomplete physical witness\n";
        return EXIT_FAILURE;
    } catch (const std::exception &failure) {
        std::cerr << failure.what() << '\n';
        return EXIT_FAILURE;
    }
}
