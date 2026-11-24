#include "envelope.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
constexpr std::uint64_t rate = 48000, frequency = 10000, block = 128;
constexpr std::uint64_t target = 24001, width = 37, length = 48000;
constexpr float threshold = 1.f / 32768.f;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

template<class Record> Record init() {
    Record out{};
    out.size = sizeof(out);
    out.abi = ES_RT_V1;
    return out;
}
void require(bool pass, const char *what) {
    if (!pass) throw std::runtime_error(what);
}

enum class Stimulus { reference, marker, vector };
struct Probe {
    std::vector<float> pcm = std::vector<float>(length);
    es_rt_lane_info lane{};
    es_rt_transport before{}, first{}, after{};
};

Probe capture(const char *entry, const char *root, Stimulus stimulus) {
    auto options = init<es_rt_config>();
    options.rate = rate;
    options.profile = ES_RT_INTERACTIVE;
    options.max_frames = block;
    options.lanes = 1;
    options.flags = ES_RT_EXPERIMENTAL;
    options.epoch = 1;
    es_rt_fleet *handle = nullptr;
    auto detail = init<es_rt_diagnostic>();
    require(es_rt_create(&options, &handle, &detail) == ES_RT_OK, detail.message);
    std::unique_ptr<es_rt_fleet, decltype(&es_rt_destroy)> fleet(handle, es_rt_destroy);
    auto voice = init<es_rt_voice_config>();
    voice.controls_policy = ES_RT_GAME;
    voice.simulation_hz = frequency;
    voice.seed = 403;
    voice.initial = {.25, 0., 0., 1, 1};
    auto command = init<es_rt_event>();
    command.epoch = options.epoch;
    command.sequence = 1;
    command.kind = ES_RT_ACTIVATE;
    command.next_voice = {1, 0, 0};
    command.duration_ms = ES_RT_DFLT_MS;
    require(es_rt_prepare_file(handle, &voice, entry, root, &command.candidate, &detail) == ES_RT_OK, detail.message);
    require(es_rt_submit(handle, &command) == ES_RT_OK, "prepare latency probe");
    command = init<es_rt_event>();
    command.epoch = options.epoch;
    command.sequence = 2;
    command.frame = (rate + 2) / 3;
    command.kind = ES_RT_VECTOR;
    command.voice = {1, 0, 0};
    command.physical = {.55, 0., 0., 1, 0};
    require(es_rt_submit(handle, &command) == ES_RT_OK, "release starter");
    if (stimulus != Stimulus::reference) {
        ++command.sequence;
        command.frame = target;
        if (stimulus == Stimulus::vector) {
            command.physical.demand = .95;
        } else {
            command.kind = ES_RT_GAIN;
            command.physical = {};
            command.gain = .5;
        }
        require(es_rt_submit(handle, &command) == ES_RT_OK, "submit latency stimulus");
    }
    if (stimulus == Stimulus::marker) {
        ++command.sequence;
        command.frame += width;
        command.gain = 1;
        require(es_rt_submit(handle, &command) == ES_RT_OK, "restore marker gain");
    }
    const auto position = [&] {
        auto out = init<es_rt_transport>();
        require(es_rt_get_transport(handle, &out) == ES_RT_OK && !out.broken, "latency transport");
        return out;
    };
    Probe probe;
    probe.before = position();
    require(es_rt_start(handle, &detail) == ES_RT_OK, detail.message);
    probe.first = position();
    for (std::uint64_t frame = 0; frame < length; frame += block) {
        const auto maximum = std::min(block, length - frame);
        const auto until = Clock::now() + 5s;
        while (position().generated < frame + maximum) {
            require(Clock::now() < until, "latency probe producer stalled");
            std::this_thread::sleep_for(100us);
        }
        auto audio = init<es_rt_audio>();
        audio.frames = audio.capacity = static_cast<std::uint32_t>(maximum);
        audio.stem[0] = probe.pcm.data() + frame;
        auto outcome = init<es_rt_block>();
        require(es_rt_process(handle, &audio, &outcome) == ES_RT_OK && outcome.first == frame &&
                outcome.pcm_frames == maximum && !outcome.fallback_frames, "latency probe PCM prefix");
        auto snapshot = init<es_rt_snapshot>();
        while (es_rt_poll_snapshot(handle, &snapshot) == ES_RT_OK) {
            require(snapshot.lane[0].fault == ES_RT_OK, "latency voice fault");
            probe.lane = snapshot.lane[0];
        }
    }
    probe.after = position();
    for (std::uint64_t seq = 1; seq <= command.sequence; ++seq) {
        auto out = init<es_rt_result>();
        require(es_rt_poll_result(handle, seq, &out) == ES_RT_OK && out.reason == ES_RT_OK &&
                (seq != 3 || out.applied == target), "latency command receipt");
    }
    require(es_rt_quiesce(handle) == ES_RT_OK, "latency probe quiesce");
    return probe;
}

bool verifyMarker(unsigned shift) {
    engine_sim::fleet::Envelope gain(1);
    gain.retarget(target + shift, 0, .5);
    auto actual = target - 1;
    while (actual < target + width && gain.at(actual) == 1.) ++actual;
    std::cout << "synthetic_marker_expected=" << target << " observed=" << actual << '\n';
    return actual == target;
}

std::uint64_t response(const Probe &baseline, const Probe &controlled) {
    for (std::uint64_t f = target; f + 8 <= length; ++f) {
        if (std::abs(controlled.pcm[f] - baseline.pcm[f]) < threshold) continue;
        unsigned observed = 0;
        for (std::uint64_t i = f; i < f + 8; ++i)
            observed += std::abs(controlled.pcm[i] - baseline.pcm[i]) >= threshold;
        if (observed >= 3) return f;
    }
    return ES_RT_NO_FRAME;
}
}

int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--inject-shift") return verifyMarker(1) ? 0 : 1;
        if (argc != 3) return 2;
        require(verifyMarker(0), "synthetic marker boundary");
        const auto baseline = capture(argv[1], argv[2], Stimulus::reference);
        const auto marker = capture(argv[1], argv[2], Stimulus::marker);
        const auto controlled = capture(argv[1], argv[2], Stimulus::vector);
        require(baseline.before.submitted == 0 && baseline.before.generated == 0 && baseline.before.sealed == 0 &&
                baseline.first.submitted == 0 && baseline.first.generated == 384 && baseline.first.sealed == 384 &&
                baseline.after.submitted == length, "retained transport origin and cursors");
        unsigned inside = 0;
        for (std::uint64_t i = 0; i < length; ++i) {
            const auto pulse = i >= target && i < target + width;
            require(marker.pcm[i] == (pulse ? .5f * baseline.pcm[i] : baseline.pcm[i]), "PCM gain pulse boundary");
            if (pulse && marker.pcm[i] != baseline.pcm[i]) ++inside;
            if (i < target) require(controlled.pcm[i] == baseline.pcm[i], "retroactive physical change");
        }
        require(inside != 0, "silent PCM marker pulse");
        constexpr auto tick = (target * frequency + rate - 1) / rate;
        constexpr auto frame = tick * rate / frequency;
        require(controlled.lane.last_latched_sequence == 3 && controlled.lane.requested_frame == target &&
                controlled.lane.latch_tick == tick && controlled.lane.latch_frame == frame, "physical tick latch");
        const auto model = response(baseline, controlled);
        require(model != ES_RT_NO_FRAME && model >= frame && model < length, "paired model response missing");
        constexpr auto estimatedQueue = 256;
        std::cout << "D_at_first=" << baseline.first.submitted << " G_at_first=" << baseline.first.generated
                  << " S_at_first=" << baseline.first.sealed << " target_frame=" << target
                  << " latch_tick=" << tick << " latch_frame=" << frame << " first_model_frame=" << model
                  << " model_threshold=" << threshold << " marker_width_frames=" << width
                  << " marker_changed_frames=" << inside << " assumed_host_queue_frames=" << estimatedQueue
                  << " example_model_dac_frame=" << model + estimatedQueue
                  << " model_settling=unmeasured physical_loopback=pending\n";
        return 0;
    } catch (const std::exception &issue) {
        std::cerr << issue.what() << '\n';
        return 1;
    }
}
