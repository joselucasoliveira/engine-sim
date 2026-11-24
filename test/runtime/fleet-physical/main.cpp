#include "owner.hpp"
#include "json_runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace engine_sim {
struct RuntimeTestAccess {
    static PullEngine &pull(Runtime &runtime) { return *runtime.m_session->pull; }
};
}

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using engine_sim::fleet::GameControls;
using Fleet = std::unique_ptr<es_rt_fleet, decltype(&es_rt_destroy)>;
using Raw = std::unique_ptr<es_runtime, decltype(&es_destroy)>;
constexpr std::uint64_t epoch = 867;
constexpr unsigned quantum = 64;

struct Point {
    unsigned at;
    es_rt_vector value;
};
struct Model {
    fs::path input, root;
    const char *name;
    bool governor;
};

void require(bool condition, const char *reason) {
    if (!condition) throw std::runtime_error(reason);
}

using Field = double es_rt_vector::*;
double line(double source, double target, unsigned elapsed, unsigned duration) {
    if (elapsed >= duration) return target;
    if (elapsed == 0) return source;
    return source + (target - source) * (static_cast<double>(elapsed) / duration);
}

std::vector<es_rt_vector> expected(const std::vector<Point> &events, unsigned count,
                                   unsigned frequency, bool governor, bool game) {
    require(!events.empty() && events.front().at == 0, "missing initial vector");
    std::vector<es_rt_vector> result(count, events.front().value);
    auto previous = events.front().value;
    const auto update = [&](const Point &point, Field field, unsigned duration) {
        const double origin = result[point.at].*field, target = point.value.*field;
        for (auto k = point.at; k < count; ++k) {
            if (field == &es_rt_vector::driven_rpm && game) {
                const auto distance = 2400.0 * static_cast<double>(k - point.at) / frequency;
                result[k].*field = distance >= std::abs(target - origin) ? target :
                    (target > origin ? origin + distance : origin - distance);
            } else {
                result[k].*field = line(origin, target, k - point.at, duration);
            }
        }
    };
    const unsigned demandTime = game && !governor ? static_cast<unsigned>(std::ceil(frequency / 100.L)) : 0u;
    const unsigned loadTime = game ? static_cast<unsigned>(std::ceil(frequency / 200.L)) : 0u;
    struct PolicyField { Field member; unsigned duration; };
    const PolicyField fields[] = {
        {&es_rt_vector::demand, demandTime}, {&es_rt_vector::load_nm, loadTime}, {&es_rt_vector::driven_rpm, 0}
    };
    for (auto it = events.begin() + 1; it != events.end(); ++it) {
        const auto &point = *it;
        if (point.at >= count) break;
        for (const auto &field : fields) {
            if (point.value.*field.member != previous.*field.member) update(point, field.member, field.duration);
        }
        for (auto k = point.at; k < count; ++k) {
            result[k].ignition = point.value.ignition;
            result[k].starter = point.value.starter;
        }
        previous = point.value;
    }
    return result;
}

std::uint64_t bits(double value) {
    std::uint64_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
void same(double have, double want, unsigned ulp = 0) {
    require(std::isfinite(have) && std::isfinite(want), "nonfinite trajectory");
    const auto a = bits(have), b = bits(want);
    if ((a >= b ? a - b : b - a) > ulp) {
        std::cerr << std::hexfloat << "scalar have=" << have << " want=" << want << std::defaultfloat << '\n';
        throw std::runtime_error("trajectory differs from independent scalar");
    }
}
void sameVector(es_rt_vector have, es_rt_vector want, unsigned ulp = 0) {
    same(have.demand, want.demand, ulp);
    same(have.load_nm, want.load_nm, ulp);
    same(have.driven_rpm, want.driven_rpm, ulp);
    require(have.ignition == want.ignition && have.starter == want.starter, "discrete edges failed to latch");
}

void curves() {
    for (const unsigned hz : {4000u, 10000u, 30000u, 44100u, 96000u}) {
        for (bool governor : {false, true}) {
            const auto quarterRamp = hz / 400;
            const auto release = hz / 28;
            const auto end = release + hz / 10;
            const std::vector<Point> script{
                {0, {.125, .5, 4., 0, 0}},
                {3, {.875, 200., 89., 1, 1}},
                {quarterRamp, {.875, 200., 89., 0, 0}},
                {quarterRamp + 1, {.625, 3.25, 14., 1, 0}},
                {quarterRamp + 9, {.625, 3.25, 14., 1, 1}},
                {release, {0., 0., 0., 0, 0}}
            };
            const auto reference = expected(script, end + 1, hz, governor, true);
            GameControls controls(script[0].value, hz, governor);
            es_rt_vector previous{};
            std::size_t i = 0;
            for (unsigned k = 0; k <= end; ++k) {
                if (i + 1 < script.size() && script[i + 1].at == k) ++i;
                const auto have = controls.at(k, script[i].value);
                sameVector(have, reference[k], 4);
                if (k >= 1) {
                    require(std::abs(have.driven_rpm - previous.driven_rpm) <=
                        2400. / hz + 32 * std::numeric_limits<double>::epsilon() * 100., "slew bound");
                }
                for (const auto &point : script) {
                    const auto elapsed = k >= point.at ? k - point.at : UINT32_MAX;
                    if (elapsed == static_cast<unsigned>(std::ceil(hz / 100.L))
                            && script[i].value.demand == point.value.demand)
                        same(have.demand, point.value.demand);
                }
                if (k == 3 || k == quarterRamp + 1 || k == release || k == end) {
                    std::cout << "F=" << hz << " governor=" << governor << " k=" << k << std::hexfloat
                              << " demand=" << have.demand << " load=" << have.load_nm << " rpm=" << have.driven_rpm
                              << std::defaultfloat << " ignition=" << have.ignition << " starter=" << have.starter << '\n';
                }
                previous = have;
            }
            sameVector(previous, script.back().value);
        }
    }
}

Fleet create(unsigned rate, unsigned frequency, bool driven, bool game, const Model &model,
             es_rt_vector first, es_rt_token &token) {
    const es_rt_config config{sizeof(config), ES_RT_V1, rate, ES_RT_BALANCED, quantum, 1, ES_RT_EXPERIMENTAL, 0, epoch};
    es_rt_fleet *fleet = nullptr;
    require(es_rt_create(&config, &fleet, nullptr) == ES_RT_OK && fleet, "create physical test fleet");
    Fleet owner(fleet, es_rt_destroy);
    const es_rt_voice_config voice{sizeof(voice), ES_RT_V1, driven ? ES_RT_DRIVEN : ES_RT_FREE, frequency,
                                   game ? ES_RT_GAME : ES_RT_RAW, 0, 403, 0, first};
    es_rt_diagnostic problem{sizeof(problem), ES_RT_V1, 0, 0, {}};
    const auto input = model.input.u8string(), root = model.root.u8string();
    require(es_rt_prepare_file(fleet, &voice, input.c_str(), root.c_str(), &token, &problem) == ES_RT_OK,
            problem.message);
    return owner;
}

void submit(es_rt_fleet &fleet, es_rt_token token, const std::vector<Point> &script) {
    for (std::size_t i = 0; i < script.size(); ++i) {
        es_rt_event value{};
        value.size = sizeof(value);
        value.abi = ES_RT_V1;
        value.epoch = epoch;
        value.sequence = i + 1;
        value.frame = script[i].at;
        if (i == 0) {
            value.kind = ES_RT_ACTIVATE;
            value.next_voice = {1, 0, 0};
            value.candidate = token;
        } else {
            value.kind = ES_RT_VECTOR;
            value.voice = {1, 0, 0};
            value.physical = script[i].value;
        }
        require(es_rt_submit(&fleet, &value) == ES_RT_OK, "submit physical timeline");
    }
}

Raw createRaw(const Model &model, unsigned rate, unsigned frequency, bool driven) {
    es_config config{};
    es_error problem{};
    require(es_config_init(ES_ABI_V2, sizeof(config), &config, &problem) == ES_OK, problem.message);
    config.output_hz = rate;
    config.simulation_hz = frequency;
    config.mode = driven ? ES_DRIVEN : ES_FREE;
    config.seed = 403;
    es_runtime *runtime = nullptr;
    require(es_create(&config, &runtime, &problem) == ES_OK, problem.message);
    Raw result(runtime, es_destroy);
    require(engine_sim::json_runtime::load_file(runtime, model.input, model.root, nullptr, &problem) == ES_OK, problem.message);
    return result;
}

void controls(es_runtime &raw, es_rt_vector value) {
    es_error problem{};
    const es_controls request{sizeof(request), ES_ABI_V2, value.ignition, value.starter,
                              value.demand, value.load_nm, value.driven_rpm};
    require(es_set_controls(&raw, &request, &problem) == ES_OK, problem.message);
}

es_telemetry telemetry(const es_runtime &raw) {
    es_telemetry result{};
    result.size = sizeof(result);
    result.abi = ES_ABI_V2;
    es_error problem{};
    require(es_get_telemetry(&raw, &result, &problem) == ES_OK, problem.message);
    return result;
}

template<class Observe>
void run(es_rt_fleet &fleet, std::vector<float> &pcm, Observe observe) {
    es_rt_diagnostic problem{sizeof(problem), ES_RT_V1, 0, 0, {}};
    require(es_rt_start(&fleet, &problem) == ES_RT_OK, problem.message);
    unsigned last = 0;
    const auto poll = [&] {
        es_rt_snapshot value{};
        value.size = sizeof(value);
        value.abi = ES_RT_V1;
        while (es_rt_poll_snapshot(&fleet, &value) == ES_RT_OK) {
            require(value.generated == last + quantum && value.dropped == 0, "test observer lost a physical snapshot");
            last += quantum;
            if (last <= pcm.size()) observe(value);
        }
    };
    for (unsigned i = 0; i < pcm.size(); i += quantum) {
        const auto timeout = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            poll();
            if (last >= i + quantum) break;
            require(std::chrono::steady_clock::now() < timeout, "producer failed to reach observed PCM");
            std::this_thread::sleep_for(100us);
        }
        es_rt_audio audio{sizeof(audio), ES_RT_V1, quantum, quantum, nullptr, {pcm.data() + i}};
        es_rt_block result{sizeof(result), ES_RT_V1, 0, 0, 0, 0};
        require(es_rt_process(&fleet, &audio, &result) == ES_RT_OK && result.first == i &&
            result.pcm_frames == quantum && result.fallback_frames == 0, "physical output span");
    }
    require(es_rt_quiesce(&fleet) == ES_RT_OK, "quiesce physical owner");
}

void checkMeasured(const es_rt_lane_info &lane, const es_telemetry &ref) {
    require(lane.frames == ref.frames && lane.ticks == ref.ticks && lane.native_frames == ref.native_frames,
            "physical reference clock counts");
    same(lane.measured_rpm, ref.measured_rpm);
    same(lane.cycle_rad, ref.cycle_rad);
    same(lane.plate_angle_rad, ref.plate_angle_rad);
    same(lane.load_reaction_nm, ref.load_reaction_nm);
    same(lane.drive_reaction_nm, ref.drive_reaction_nm);
}

std::vector<Point> physicalScript(bool driven) {
    return {
        {0, {.25, 0, 0, 1, static_cast<std::uint32_t>(!driven)}},
        {129, {.8, 1.25, driven ? 160. : 0., 1, static_cast<std::uint32_t>(!driven)}},
        {259, {.8, 1.25, driven ? 160. : 0., 0, static_cast<std::uint32_t>(!driven)}},
        {319, {.45, .75, driven ? 160. : 0., 1, static_cast<std::uint32_t>(!driven)}},
        {2337, {.65, 1.5, driven ? 45. : 0., 1, static_cast<std::uint32_t>(!driven)}},
        {4829, {.4, .25, driven ? 45. : 0., 0, 0}},
        {5201, {.6, 0., 0., 1, 0}}
    };
}

void capturePcm(const std::string &name, const std::vector<float> &samples) {
    const char *directory = std::getenv("ES_FLEET_EVIDENCE");
    if (!directory || !*directory) return;
    std::ofstream file(fs::u8path(directory) / (name + ".f32le"), std::ios::binary);
    file.write(reinterpret_cast<const char *>(samples.data()),
               static_cast<std::streamsize>(samples.size() * sizeof(float)));
    require(static_cast<bool>(file), "write physical PCM evidence");
}

void response(const Model &model, bool driven, bool game) {
    constexpr unsigned rate = 44100;
    const bool longer = model.governor && driven;
    const unsigned frames = longer ? 32768 : 8192;
    auto script = physicalScript(driven);
    if (longer) {
        for (unsigned i : {1u, 2u, 3u}) script[i].value.driven_rpm = 600.;
        script[4].at = 18001;
        script[5].at = 24577;
        script[4].value.driven_rpm = script[5].value.driven_rpm = 220.;
        script[6].at = 27001;
    }
    const auto vector = expected(script, frames, rate, model.governor, game);
    auto raw = createRaw(model, rate, rate, driven);
    std::vector<float> expectedPcm(frames), actualPcm(frames);
    std::vector<es_telemetry> states;
    states.reserve(frames / quantum);
    for (unsigned i = 0; i < frames; ++i) {
        controls(*raw, vector[i]);
        es_error problem{};
        std::uint32_t written = 0;
        require(es_render(raw.get(), 1, expectedPcm.data() + i, 1, &written, &problem) == ES_OK && written == 1,
                problem.message);
        if ((i + 1) % quantum == 0) states.push_back(telemetry(*raw));
    }
    es_rt_token token{};
    auto owner = create(rate, rate, driven, game, model, script[0].value, token);
    submit(*owner, token, script);
    run(*owner, actualPcm, [&](const es_rt_snapshot &snapshot) {
        const auto &lane = snapshot.lane[0];
        const auto frame = snapshot.generated - 1;
        const auto &rawState = states[frame / quantum];
        const auto point = std::upper_bound(script.begin(), script.end(), frame,
            [](std::uint64_t f, const Point &event) { return f < event.at; }) - 1;
        const auto sequence = static_cast<std::uint64_t>(point - script.begin()) + 1;
        require(lane.state == ES_RT_PLAY && lane.fault == ES_RT_OK && lane.last_sequence == sequence &&
                lane.last_latched_sequence == sequence && lane.latch_tick == point->at &&
                lane.latch_frame == point->at && lane.requested_frame == point->at, "physical latch metadata");
        sameVector(lane.requested, point->value);
        sameVector(lane.applied, vector[frame]);
        checkMeasured(lane, rawState);
        if (driven) same(lane.applied.driven_rpm, rawState.applied_driven_rpm);
        if (frame < 512 || (frame % 1024 == 1023)) {
            std::cout << model.name << " policy=" << (game ? "GAME" : "RAW") << " mode=" << driven
                      << " frame=" << frame << " event=" << sequence << " latch=" << lane.latch_tick
                      << std::hexfloat << " target-rpm=" << lane.requested.driven_rpm
                      << " applied-rpm=" << lane.applied.driven_rpm << " measured=" << lane.measured_rpm
                      << " cycle=" << lane.cycle_rad << " load-reaction=" << lane.load_reaction_nm
                      << std::defaultfloat << '\n';
        }
    });
    require(std::memcmp(expectedPcm.data(), actualPcm.data(), frames * sizeof(float)) == 0,
            "physical PCM differs from independent v2 schedule");
    const auto name = std::string(model.name) + (driven ? "-driven" : "-free") + (game ? "-game" : "-raw");
    capturePcm(name + "-v2", expectedPcm);
    capturePcm(name + "-fleet", actualPcm);
    const auto useful = std::count_if(actualPcm.begin(), actualPcm.end(), [](float v) { return v != 0; });
    require(useful != 0, "physical comparison produced no useful PCM");
    std::cout << model.name << " mode=" << driven << " policy=" << (game ? "GAME" : "RAW")
              << " matched-pcm=" << frames << " nonzero=" << useful << '\n';
}

void fastTicks(const Model &model) {
    constexpr unsigned fs = 44100, frequency = 96000, frames = 512;
    const std::vector<Point> script{
        {0, {.1, 0., 0., 1, 1}}, {1, {.9, .5, 0., 1, 1}},
        {63, {.6, 2., 0., 0, 0}}, {129, {.2, .1, 0., 1, 0}}
    };
    auto tickScript = script;
    for (auto &point : tickScript) point.at = (point.at * frequency + fs - 1) / fs;
    const auto oracle = expected(tickScript, 2048, frequency, model.governor, true);
    std::array<es_rt_vector, 4096> actual{};
    unsigned captured = 0;
    es_rt_token token{};
    auto owner = create(fs, frequency, false, true, model, script[0].value, token);
    auto &voice = *owner->cells[token.cell].voice;
    engine_sim::RuntimeTestAccess::pull(voice.runtime).setTickCommands([&] {
        if (captured < actual.size()) actual[captured++] = voice.applied;
    }, {});
    submit(*owner, token, script);
    std::vector<float> pcm(frames);
    unsigned checked = 0;
    run(*owner, pcm, [&](const es_rt_snapshot &snapshot) {
        const auto &lane = snapshot.lane[0];
        require(lane.frames == snapshot.generated && lane.ticks == snapshot.generated * frequency / fs &&
                lane.fault == ES_RT_OK, "fast tick case failed");
        checked = static_cast<unsigned>(lane.ticks);
    });
    require(checked == frames * frequency / fs && captured >= checked, "fast tick witness extent");
    for (unsigned i = 0; i < checked; ++i) sameVector(actual[i], oracle[i], 4);
    std::cout << "fast F=" << frequency << " Fs=" << fs << " observed-ticks=" << checked << " last-first-tick="
              << tickScript.back().at << '\n';
}

void saturation(const Model &model) {
    constexpr unsigned rate = 44100, transition = 2205, complete = 2206, frames = 2304;
    const std::vector<Point> script{{0, {.2, 100, 0, 1, 0}}, {transition, {.2, 100, 3600, 1, 0}}};
    auto raw = createRaw(model, rate, 0, true);
    auto isolated = createRaw(model, rate, 0, true);
    controls(*raw, script[0].value);
    controls(*isolated, script[0].value);
    std::vector<float> reference(frames), actual(frames);
    std::array<float, 64> rawTail{};
    es_error detail{};
    std::uint32_t count = 0;
    require(es_render(isolated.get(), complete, reference.data(), complete, &count, &detail) == ES_OK && count == complete,
            detail.message);
    const auto last = static_cast<double>(reference[complete - 1]);
    for (unsigned i = 0; i < 31; ++i) reference[complete + i] = static_cast<float>(last * (31 - i) / 32.);
    require(es_render(raw.get(), transition, actual.data(), transition, &count, &detail) == ES_OK && count == transition,
            detail.message);
    controls(*raw, script[1].value);
    require(es_render(raw.get(), 64, rawTail.data(), 64, &count, &detail) == ES_RENDER_FAULT && count == 1 &&
        std::string_view(detail.message).find("authority saturated") != std::string_view::npos,
        "v2 did not reproduce the original RAW shaft fault");

    es_rt_token token{};
    auto fleet = create(rate, 0, true, false, model, script[0].value, token);
    submit(*fleet, token, script);
    unsigned observed = 0;
    run(*fleet, actual, [&](const es_rt_snapshot &snapshot) {
        if (snapshot.generated < complete) return;
        const auto &lane = snapshot.lane[0];
        require(lane.state == ES_RT_FAILED && lane.fault == ES_RT_FAULT, "RAW authority fault was hidden");
        checkMeasured(lane, telemetry(*isolated));
        ++observed;
    });
    require(observed && std::memcmp(reference.data(), actual.data(), frames * sizeof(float)) == 0,
            "RAW fault changed the exact prefix or its initialized suffix");
    capturePcm("CH750-raw-saturation-v2", reference);
    capturePcm("CH750-raw-saturation-fleet", actual);
    require(fleet->cells[token.cell].voice->runtime.processingFailure() == engine_sim::ProcessingStatus::drive_saturation,
            "fleet RAW mode changed numerical fault identity");
    std::cout << model.name << " RAW requested-rpm=3600 exact-prefix=" << complete
              << " fault=" << engine_sim::processingMessage(engine_sim::ProcessingStatus::drive_saturation) << '\n';
}
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    try {
        const std::string_view mode = argv[1];
        const auto source = fs::absolute(argv[2]);
        const auto fixture = source / "examples/runtime/fixture";
        const Model single{fixture / "main.json", fixture, "Single", false};
        const Model kohler{source / "assets/engines/atg-video-1/02_kohler_ch750.json", source, "CH750", true};
        if (mode == "curves") {
            curves();
            fastTicks(single);
            response(single, false, false);
        } else if (mode == "response") {
            for (const auto &model : {single, kohler}) {
                response(model, false, true);
                response(model, true, true);
            }
            saturation(kohler);
        } else {
            return 2;
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
