#ifndef ENGINE_SIM_DESKTOP_RUN_H
#define ENGINE_SIM_DESKTOP_RUN_H
#include "application_settings.h"
#include "desktop_audio_pacer.h"
#include "desktop_host.h"
#include <chrono>
#include <functional>

namespace desktop {
struct RunOptions {
    engine_sim::ResourceOptions resources;
    bool smoke = false, virtualAudio = false, help = false;
    unsigned frames = 240;
    std::filesystem::path report;
};
RunOptions parseRunOptions(const std::vector<std::string> &arguments);
struct RunEvidence {
    RunOptions options;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::array<std::uint64_t, static_cast<std::size_t>(Key::Count)> edges{}, held{};
    std::uint64_t frames = 0, modelPackets = 0, generatedPackets = 0, textPackets = 0;
    std::uint64_t sourceFrames = 0, nonzeroSource = 0, sourceDigest = 14695981039346656037ULL;
    std::uint64_t processCalls = 0, simulationSteps = 0, screenshots = 0, coloredPixels = 0;
    std::uint64_t audioConsumedInput = 0, audioPumpedSource = 0, silenceFrames = 0, queueMaximum = 0;
    std::uint64_t nonzeroFedToBackend = 0;
    std::uint64_t modelDigest = 0;
    std::uint64_t preparedFrames = 0, preparedDigest = 14695981039346656037ULL;
    std::uint64_t acceptedReloads = 0;
    double peakRpm = 0;
    int cylinders = 0;
    bool resized = false, tornDown = false, success = false, throttleChanged = false;
    std::string videoDriver, renderer, audioDriver, audioDevice, error;
    std::string engineName;
    ApplicationSettings settings;
    AudioPacer::Statistics pacing;
    int deviceFrequency = 0, deviceChannels = 0, deviceQuantum = 0;
    unsigned deviceFormat = 0;
    std::filesystem::path resolvedModel, resourceRoot;
    std::function<void(unsigned)> onFrame;
    void beforeFrame(Host &host);
    void afterFrame(Host &host);
    bool key(Key key, bool value, bool edge);
    void source(const AudioSegments &segments);
    void validate() const;
    void writeReport() const;
};
int runApplication(const std::vector<std::string> &arguments,
                   const std::function<std::unique_ptr<Host>(const RunOptions &)> &factory);
std::unique_ptr<Host> makeSdlHost(const RunOptions &options);
}
#endif
