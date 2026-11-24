#include "envelope.hpp"
#include "engine_sim/runtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using engine_sim::fleet::Envelope;
using engine_sim::fleet::fadeFrames;
using engine_sim::fleet::scaleStem;
using Owner = std::unique_ptr<es_rt_fleet, decltype(&es_rt_destroy)>;
using Raw = std::unique_ptr<es_runtime, decltype(&es_destroy)>;

constexpr unsigned rate = 44100, maximum = 128, total = 4608;
constexpr std::uint64_t epoch = 552;
constexpr es_rt_vector initial{.25, 0, 0, 1, 1}, later{.65, .375, 0, 1, 1};

void require(bool valid, const char *message) {
    if (!valid) throw std::runtime_error(message);
}
bool same(float a, float b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
bool same(double a, double b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

es_rt_event event(unsigned sequence, unsigned at, unsigned kind, unsigned milliseconds = 0) {
    es_rt_event out{};
    out.size = sizeof(out); out.abi = ES_RT_V1; out.kind = kind; out.duration_ms = milliseconds;
    out.epoch = epoch; out.sequence = sequence; out.frame = at;
    if (kind != ES_RT_MASTER && kind != ES_RT_ACTIVATE) out.voice = {1, 0, 0};
    return out;
}

void synthetic() {
    require(fadeFrames(event(1, 0, ES_RT_MUTE, ES_RT_DFLT_MS), rate) == 221 &&
            fadeFrames(event(1, 0, ES_RT_GAIN, 100), 48000) == 4800, "whole frame duration");
    std::array<double, 25> gain{{
        0., 0.125, 0.25, 0.375, 0.28125, 0.1875, 0.09375, 0., 0., 1., 1., 1., 1.,
        .875, .75, .625, .5, .5, .5, .5, .5, .5, .5, .5, .5
    }};
    for (unsigned partition = 1; partition <= maximum; ++partition) {
        Envelope envelope;
        for (unsigned start = 0; start < gain.size(); start += partition) {
            const auto end = std::min<unsigned>(start + partition, gain.size());
            for (auto frame = start; frame < end; ++frame) {
                if (frame == 0) envelope.retarget(0, 8, 1.);
                if (frame == 3) envelope.retarget(3, 4, 0.);
                if (frame == 9) {
                    envelope.retarget(9, 32, 1.);
                    envelope.retarget(9, 0, 1.);
                }
                if (frame == 12) envelope.retarget(12, 4, 0.5);
                if (frame == 14 && envelope.target() != 0.5) envelope.retarget(14, 99, 0.5);
                require(same(envelope.at(frame), gain[frame]), "absolute envelope changed with partition");
                for (float source : {1.f, frame < 12 ? -.5f : .75f,
                        static_cast<float>(std::sin(static_cast<double>(frame)))}) {
                    const float want = static_cast<float>(static_cast<double>(source) * gain[frame]);
                    require(same(scaleStem(source, .5, envelope.at(frame)),
                                 gain[frame] == 0. ? 0.f : static_cast<float>(want * 0.5f)),
                            "finite source scaled to an unexpected sample");
                }
            }
        }
        require(same(scaleStem(-0.f, 1., 1.), -0.f) && same(scaleStem(-0.f, 1., 0.), 0.f),
                "unity bypass and exact gate zero");
    }
    std::cout << "synthetic partitions=128 signals=constant,step,sinusoid exact-endpoints=1\n";
}

Owner owner(const fs::path &model, const fs::path &root, es_rt_token &token) {
    const es_rt_config config{sizeof(config), ES_RT_V1, rate, ES_RT_BALANCED, maximum, 1, ES_RT_EXPERIMENTAL, 0, epoch};
    es_rt_fleet *value = nullptr;
    require(es_rt_create(&config, &value, nullptr) == ES_RT_OK && value, "create lifecycle owner");
    Owner fleet(value, es_rt_destroy);
    const es_rt_voice_config voice{sizeof(voice), ES_RT_V1, ES_RT_FREE, rate, ES_RT_RAW, 0, 403, 0, initial};
    const auto path = model.u8string(), assets = root.u8string();
    es_rt_diagnostic problem{sizeof(problem), ES_RT_V1, 0, 0, {}};
    require(es_rt_prepare_file(value, &voice, path.c_str(), assets.c_str(), &token, &problem) == ES_RT_OK,
            problem.message);
    return fleet;
}

Raw raw(const fs::path &model, const fs::path &root) {
    es_config config{};
    es_error error{};
    require(es_config_init(ES_ABI_V2, sizeof(config), &config, &error) == ES_OK, error.message);
    config.output_hz = rate;
    config.simulation_hz = rate;
    config.seed = 403;
    es_runtime *value = nullptr;
    require(es_create(&config, &value, &error) == ES_OK, error.message);
    Raw runtime(value, es_destroy);
    const std::string path = model.u8string(), assets = root.u8string();
    require(es_load_json_file(value, path.c_str(), assets.c_str(), nullptr, &error) == ES_OK, error.message);
    return runtime;
}

void physical(es_runtime &runtime, es_rt_vector vector) {
    es_error error{};
    const es_controls controls{sizeof(controls), ES_ABI_V2, vector.ignition, vector.starter,
                               vector.demand, vector.load_nm, vector.driven_rpm};
    require(es_set_controls(&runtime, &controls, &error) == ES_OK, error.message);
}

std::vector<es_rt_event> events(es_rt_token token) {
    auto result = std::vector<es_rt_event>{
        event(1, 0, ES_RT_ACTIVATE),
        event(2, 2017, ES_RT_MUTE, 2),
        event(3, 2059, ES_RT_MUTE, 3),
        event(4, 2513, ES_RT_PAUSE),
        event(5, 2531, ES_RT_VECTOR),
        event(6, 2581, ES_RT_GAIN, 2),
        event(7, 2689, ES_RT_UNMUTE, 1),
        event(8, 2913, ES_RT_RESUME, 1),
        event(9, 2945, ES_RT_PAUSE, 2),
        event(10, 2945, ES_RT_RESUME),
        event(11, 3401, ES_RT_PAUSE, 2),
        event(12, 3577, ES_RT_RESUME, 2),
        event(13, 3671, ES_RT_MASTER, ES_RT_DFLT_MS),
        event(14, 3771, ES_RT_MASTER, 100),
        event(15, 3901, ES_RT_GAIN),
        event(16, 4301, ES_RT_STOP, 2),
        event(17, 4310, ES_RT_MUTE)
    };
    result[0].candidate = token;
    result[0].next_voice = {1, 0, 0};
    result[4].physical = later;
    result[5].gain = .25;
    result[12].gain = result[13].gain = .8;
    result[14].gain = 1.;
    return result;
}

double ramp(double from, double to, unsigned frame, unsigned start, unsigned duration) {
    if (frame <= start) return from;
    if (frame >= start + duration) return to;
    return from + (to - from) * (static_cast<double>(frame - start) / duration);
}
double gate(unsigned frame) {
    if (frame < 2106) return ramp(1., 0., frame, 2017, 89);
    if (frame < 2913) return 0.;
    if (frame < 2945) return ramp(0., 1., frame, 2913, 45);
    if (frame < 3490) return ramp(1., 0., frame, 3401, 89);
    if (frame < 3577) return 0.;
    if (frame < 3666) return ramp(0., 1., frame, 3577, 89);
    if (frame < 4390) return ramp(1., 0., frame, 4301, 89);
    return 0.;
}
bool advances(unsigned frame) {
    return frame < 2513 || (frame >= 2913 && frame < 3490) || (frame >= 3577 && frame < 4390);
}

struct Match {
    std::array<float, total> source{}, stem{}, mix{};
    std::array<es_telemetry, total / 64> states{};
    unsigned soundedWhileMuted = 0;
    unsigned soundedAfterResume = 0;
    unsigned tailBeforeStop = 0;
};

Match reference(const fs::path &model, const fs::path &root) {
    auto runtime = raw(model, root);
    physical(*runtime, initial);
    Match ref;
    for (unsigned frame = 0; frame < total; ++frame) {
        if (frame == 2531) physical(*runtime, later);
        es_error error{};
        if (advances(frame)) {
            std::uint32_t written = 0;
            require(es_render(runtime.get(), 1, &ref.source[frame], 1, &written, &error) == ES_OK && written == 1,
                    error.message);
        }
        const auto a = gate(frame);
        const auto g = frame >= 3901 ? 1. : ramp(1., .25, frame, 2581, 89);
        const auto master = ramp(.5, .8, frame, 3671, 221);
        ref.stem[frame] = a == 0 ? 0.f : static_cast<float>(static_cast<double>(ref.source[frame]) * g * a);
        ref.mix[frame] = static_cast<float>(static_cast<double>(ref.stem[frame]) * master);
        if (ref.source[frame] != 0.f) {
            if (frame >= 2106 && frame < 2513) ++ref.soundedWhileMuted;
            if (frame >= 3577 && frame < 4301) ++ref.soundedAfterResume;
            if (frame >= 4301 && frame < 4390) ++ref.tailBeforeStop;
        }
        if ((frame + 1) % 64 == 0) {
            auto &state = ref.states[frame / 64];
            state.size = sizeof(state); state.abi = ES_ABI_V2;
            require(es_get_telemetry(runtime.get(), &state, &error) == ES_OK, error.message);
        }
    }
    require(ref.soundedWhileMuted && ref.soundedAfterResume && ref.tailBeforeStop,
            "IR schedule did not exercise muted, resumed and stopping signal");
    return ref;
}

void checkTicket(es_rt_fleet &fleet, const es_rt_event &command,
                 std::uint32_t reason, std::uint64_t applied, std::uint64_t ended) {
    es_rt_result result{};
    result.size = sizeof(result); result.abi = ES_RT_V1;
    require(es_rt_poll_result(&fleet, command.sequence, &result) == ES_RT_OK &&
            result.sequence == command.sequence && result.kind == command.kind && result.requested == command.frame &&
            result.reason == reason && result.applied == applied && result.ended == ended, "lifecycle ticket endpoint");
    require(es_rt_poll_result(&fleet, command.sequence, &result) == ES_RT_STALE, "terminal ticket consumed twice");
}

void partition(const fs::path &model, const fs::path &root, const Match &ref,
               const std::array<unsigned, 4> &pattern, bool report) {
    es_rt_token token{};
    auto fleet = owner(model, root, token);
    const auto script = events(token);
    const auto submit = [&](const es_rt_event &command) {
        require(es_rt_submit(fleet.get(), &command) == ES_RT_OK, "submit lifecycle event");
    };
    for (const auto &command : script) submit(command);
    require(es_rt_start(fleet.get(), nullptr) == ES_RT_OK, "start lifecycle render");

    unsigned generated = 0, snapshots = 0;
    const auto poll = [&] {
        es_rt_snapshot value{};
        value.size = sizeof(value); value.abi = ES_RT_V1;
        while (es_rt_poll_snapshot(fleet.get(), &value) == ES_RT_OK) {
            require(value.generated == generated + 64 && value.dropped == 0, "snapshot continuity");
            generated += 64;
            if (generated > total) continue;
            ++snapshots;
            const auto &lane = value.lane[0];
            const auto &state = ref.states[generated / 64 - 1];
            if (generated >= 4390) {
                require(lane.state == ES_RT_VACANT && !lane.voice.generation, "retired lane remained audible");
                continue;
            }
            require(lane.voice.generation == 1 && lane.frames == state.frames && lane.ticks == state.ticks &&
                    lane.native_frames == state.native_frames && same(lane.cycle_rad, state.cycle_rad) &&
                    same(lane.measured_rpm, state.measured_rpm) && same(lane.plate_angle_rad, state.plate_angle_rad),
                    "lifecycle changed the paired v2 engine history");
            if (generated >= 2560 && generated <= 2880) {
                require(lane.state == ES_RT_PAUSED && lane.frames == 2513 && lane.latch_frame == 0 &&
                        lane.requested_frame == 2531 && lane.gate == 0., "paused trajectory advanced without a tick");
            }
            if (generated == 2112) require(lane.state == ES_RT_MUTED && lane.frames == 2112,
                    "muted voice skipped frames");
            if (generated == 2944) require(lane.state == ES_RT_ATTACK && lane.latch_frame == 2913 &&
                    lane.latch_tick == 2513, "retained vector did not latch after resume");
            if (report && (generated == 2112 || generated == 2560 || generated == 2944 || generated == 3520)) {
                std::cout << "G=" << generated << " state=" << lane.state << " local=" << lane.frames << std::hexfloat
                          << " phase=" << lane.cycle_rad << " gate=" << lane.gate << std::defaultfloat << '\n';
            }
        }
    };
    for (unsigned first = 0, turn = 0; first < total; ++turn) {
        const auto length = std::min(total - first, pattern[turn % pattern.size()]);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            poll();
            if (generated >= first + length) break;
            require(std::chrono::steady_clock::now() < deadline, "lifecycle producer did not refill");
            std::this_thread::sleep_for(100us);
        }
        std::array<float, maximum + 2> stem{}, mix{};
        stem.fill(31.f); mix.fill(31.f);
        es_rt_audio audio{sizeof(audio), ES_RT_V1, length, maximum, mix.data() + 1, {stem.data() + 1}};
        es_rt_block status{sizeof(status), ES_RT_V1, 0, 0, 0, 0};
        require(es_rt_process(fleet.get(), &audio, &status) == ES_RT_OK && status.first == first &&
                status.pcm_frames == length && status.fallback_frames == 0, "lifecycle callback");
        require(same(stem.front(), 31.f) && same(stem[length + 1], 31.f) &&
                same(mix.front(), 31.f) && same(mix[length + 1], 31.f), "lifecycle overwritten canaries");
        for (unsigned i = 0; i < length; ++i) {
            if (!same(stem[1+i], ref.stem[first+i]) || !same(mix[1+i], ref.mix[first+i])) {
                std::cerr << "frame=" << first+i << " actual stem=" << stem[1+i] << " expected=" << ref.stem[first+i]
                          << " actual mix=" << mix[1+i] << " expected=" << ref.mix[first+i] << '\n';
                throw std::runtime_error("exact lifecycle PCM");
            }
        }
        first += length;
    }
    require(snapshots == total / 64, "missing lifecycle telemetry interval");
    require(es_rt_quiesce(fleet.get()) == ES_RT_OK, "quiesce after stop");
    constexpr std::array<std::uint64_t, 17> ends{
        0, 2106, 2059, 2513, 2531, 2581, 2689, 2945, 2945, 2945, 3490, 3666, 3671, 3771, 3901, 4390, ES_RT_NO_FRAME
    };
    for (unsigned i = 0; i < script.size(); ++i) {
        const auto reason = i == 16 ? ES_RT_BUSY : (i == 7 || i == 8 ? ES_RT_INTERRUPTED : ES_RT_OK);
        checkTicket(*fleet, script[i], reason, reason == ES_RT_BUSY ? ES_RT_NO_FRAME : script[i].frame, ends[i]);
    }
    es_rt_token returned{};
    es_rt_lane_info info{};
    require(es_rt_collect(fleet.get(), &returned, &info) == ES_RT_OK && returned.cell == token.cell &&
            info.voice.generation == 1 && info.frames == ref.states.back().frames &&
            info.ticks == info.frames && info.last_sequence == 16, "stop performed hidden engine work");
    require(es_rt_collect(fleet.get(), &returned, &info) == ES_RT_EMPTY, "voice returned twice");
    if (report) std::cout << "stopped frame=4390 local=" << info.frames << " receipts=" << script.size() << '\n';
}

void canceledAttack(const fs::path &model, const fs::path &root) {
    es_rt_token token{};
    auto fleet = owner(model, root, token);
    auto activate = event(1, 0, ES_RT_ACTIVATE, 100);
    activate.candidate = token;
    activate.next_voice = {1, 0, 0};
    require(es_rt_submit(fleet.get(), &activate) == ES_RT_OK && es_rt_start(fleet.get(), nullptr) == ES_RT_OK,
            "start long attack");
    require(es_rt_quiesce(fleet.get()) == ES_RT_OK, "quiesce mid-attack");
    checkTicket(*fleet, activate, ES_RT_CANCELED, 0, ES_RT_NO_FRAME);
    es_rt_lane_info info{};
    require(es_rt_collect(fleet.get(), &token, &info) == ES_RT_OK && info.frames == 512 && info.voice.generation == 1,
            "canceling a running attack lost active cell");
}

void lifecycle(const fs::path &model) {
    const auto root = model.parent_path();
    const auto expected = reference(model, root);
    partition(model, root, expected, {1, 63, 64, 65}, true);
    partition(model, root, expected, {128, 65, 63, 1}, false);
    partition(model, root, expected, {63, 63, 63, 63}, false);
    canceledAttack(model, root);
    std::cout << "actual PCM partitions=3 frames=" << total << " muted-source=" << expected.soundedWhileMuted
              << " resumed-source=" << expected.soundedAfterResume << " stop-tail-source=" << expected.tailBeforeStop << '\n';
}
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    try {
        const auto model = fs::absolute(argv[2]);
        const std::string_view mode = argv[1];
        if (mode == "shape") synthetic();
        else if (mode == "real") lifecycle(model);
        else return 2;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
