#include "owner.hpp"
#include "json_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace engine_sim {
struct RuntimeTestAccess {
    static PullEngine &pull(Runtime &runtime) { return *runtime.m_session->pull; }
    static Simulator &simulator(Runtime &runtime) { return *runtime.m_session->prepared.session->simulator; }
};
}

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using engine_sim::RuntimeTestAccess;
using Owner = std::unique_ptr<es_rt_fleet, decltype(&es_rt_destroy)>;
using Raw = std::unique_ptr<es_runtime, decltype(&es_destroy)>;
constexpr std::uint64_t epoch = 119;
constexpr float sentinel = -27.f;
constexpr es_rt_vector initial{.25, 0, 0, 1, 1};
constexpr es_rt_vector vector{.65, .375, 0, 1, 1};

void require(bool valid, const char *message) {
    if (!valid) throw std::runtime_error(message);
}

template<class Condition>
void await(Condition ready, const char *failure) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!ready()) {
        require(std::chrono::steady_clock::now() < deadline, failure);
        std::this_thread::sleep_for(100us);
    }
}

template<class Body>
void audioThread(Body body) {
    std::exception_ptr failure;
    std::thread audio([&] {
        try {
            std::cout << "A thread=" << std::this_thread::get_id() << " first-entry\n";
            body();
        } catch (...) {
            failure = std::current_exception();
        }
    });
    audio.join();
    if (failure) std::rethrow_exception(failure);
}

Owner create(unsigned rate, unsigned maximum, unsigned profile, unsigned lanes = 2) {
    es_rt_config config{sizeof(config), ES_RT_V1, rate, profile, maximum, lanes, ES_RT_EXPERIMENTAL, 0, epoch};
    es_rt_fleet *owner = nullptr;
    require(es_rt_create(&config, &owner, nullptr) == ES_RT_OK && owner, "create owner");
    return {owner, es_rt_destroy};
}

es_rt_token prepare(es_rt_fleet &fleet, const fs::path &model, unsigned hz) {
    es_rt_voice_config voice{sizeof(voice), ES_RT_V1, ES_RT_FREE, hz, ES_RT_RAW, 0, 403, 0, initial};
    const auto path = model.u8string(), root = model.parent_path().u8string();
    es_rt_token token{};
    es_rt_diagnostic detail{sizeof(detail), ES_RT_V1, 0, 0, {}};
    require(es_rt_prepare_file(&fleet, &voice, path.c_str(), root.c_str(), &token, &detail) == ES_RT_OK,
            detail.message);
    return token;
}

es_rt_event event(std::uint64_t sequence, std::uint64_t frame, unsigned kind) {
    es_rt_event value{};
    value.size = sizeof(value);
    value.abi = ES_RT_V1;
    value.epoch = epoch;
    value.kind = kind;
    value.sequence = sequence;
    value.frame = frame;
    return value;
}

void activate(es_rt_fleet &fleet, es_rt_token token) {
    auto command = event(1, 0, ES_RT_ACTIVATE);
    command.candidate = token;
    command.next_voice = {1, 1, 0};
    require(es_rt_submit(&fleet, &command) == ES_RT_OK, "queue startup activation");
}

void checkResult(es_rt_fleet &fleet, std::uint64_t sequence, std::uint64_t frame) {
    es_rt_result reply{};
    reply.size = sizeof(reply);
    reply.abi = ES_RT_V1;
    require(es_rt_poll_result(&fleet, sequence, &reply) == ES_RT_OK &&
        reply.reason == ES_RT_OK && reply.epoch == epoch && reply.sequence == sequence &&
        reply.requested == frame && reply.applied == frame && reply.ended == frame, "producer ticket receipt");
}

es_rt_transport transport(es_rt_fleet &fleet) {
    es_rt_transport out{};
    out.size = sizeof(out);
    out.abi = ES_RT_V1;
    require(es_rt_get_transport(&fleet, &out) == ES_RT_OK && out.epoch == epoch, "producer transport");
    return out;
}

es_rt_transport observe(es_rt_fleet &fleet, const char *phase) {
    const auto data = transport(fleet);
    std::cout << phase << " D=" << data.submitted << " G=" << data.generated << " S=" << data.sealed
              << " low=" << data.low_frames << " high=" << data.high_frames << " storage=" << data.storage_frames
              << " broken=" << data.broken << " returned=" << data.returned_cells << '\n';
    return data;
}

Raw raw(const fs::path &model, unsigned rate) {
    es_config config{};
    es_error error{};
    require(es_config_init(ES_ABI_V2, sizeof(config), &config, &error) == ES_OK, error.message);
    config.output_hz = rate;
    config.simulation_hz = rate;
    config.mode = ES_FREE;
    config.seed = 403;
    es_runtime *runtime = nullptr;
    require(es_create(&config, &runtime, &error) == ES_OK, error.message);
    Raw result(runtime, es_destroy);
    require(engine_sim::json_runtime::load_file(runtime, model, model.parent_path(), nullptr, &error) == ES_OK,
            error.message);
    return result;
}

void rawControls(es_runtime &raw, es_rt_vector value) {
    es_error error{};
    const es_controls controls{sizeof(controls), ES_ABI_V2, value.ignition, value.starter,
                               value.demand, value.load_nm, value.driven_rpm};
    require(es_set_controls(&raw, &controls, &error) == ES_OK, error.message);
}

void renderRaw(es_runtime &raw, float *output, unsigned frames) {
    es_error error{};
    std::uint32_t written = 0;
    require(es_render(&raw, frames, output, frames, &written, &error) == ES_OK, error.message);
    require(written == frames, "raw reference partial render");
}

bool equalSample(float a, float b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

void consume(es_rt_fleet &fleet, const std::vector<float> &expected, unsigned first,
             unsigned frames, bool fallback = false) {
    std::array<float, 1026> mix, inactive, active;
    for (auto *plane : {&mix, &inactive, &active}) plane->fill(sentinel);
    es_rt_audio audio{sizeof(audio), ES_RT_V1, frames, 1024, mix.data() + 1,
                      {inactive.data() + 1, active.data() + 1}};
    es_rt_block result{sizeof(result), ES_RT_V1, 0, 0, 0, 0};
    const auto status = es_rt_process(&fleet, &audio, &result);
    require(status == (fallback ? ES_RT_BROKEN : ES_RT_OK), "callback span status");
    require(result.first == first && result.epoch == epoch && result.pcm_frames == (fallback ? 0u : frames) &&
            result.fallback_frames == (fallback ? frames : 0u), "callback span source counts");
    require(fallback || first + frames <= expected.size(), "missing PCM reference for expected rendered frames");
    for (unsigned i = 0; i < frames; ++i) {
        const auto sample = fallback ? 0.f : expected[first + i];
        const auto mixed = static_cast<float>(std::clamp(static_cast<double>(0.f + sample) * 0.5, -1., 1.));
        require(equalSample(active[i + 1], sample) && equalSample(mix[i + 1], mixed) &&
                equalSample(inactive[i + 1], 0.f), "producer PCM or fallback differs from reference");
    }
    for (const auto *plane : {&mix, &inactive, &active}) {
        require(plane->front() == sentinel &&
            std::all_of(plane->begin() + frames + 1, plane->end(), [](float v) { return v == sentinel; }),
            "callback wrote outside its span");
    }
}

void start(es_rt_fleet &fleet) {
    es_rt_diagnostic detail{sizeof(detail), ES_RT_V1, 0, 0, {}};
    require(es_rt_start(&fleet, &detail) == ES_RT_OK, detail.message);
}

void expectStopped(es_rt_fleet &fleet) {
    float sample = sentinel;
    es_rt_audio audio{sizeof(audio), ES_RT_V1, 1, 1, &sample, {}};
    es_rt_block result{sizeof(result), ES_RT_V1, 2, 3, 4, 5};
    require(es_rt_process(&fleet, &audio, &result) == ES_RT_STATE && sample == sentinel &&
            result.first == 5 && result.pcm_frames == 2, "callback outside the started epoch touched output");
}

void horizon(const fs::path &model, unsigned rate) {
    constexpr unsigned maximum = 1024, high = 4 * maximum;
    constexpr unsigned change = 2049, detached = 9001, end = 12288;
    auto isolated = raw(model, rate);
    std::vector<float> expected(end);
    rawControls(*isolated, initial);
    renderRaw(*isolated, expected.data(), change);
    rawControls(*isolated, vector);
    renderRaw(*isolated, expected.data() + change, detached - change);
    const auto useful = std::count_if(expected.begin(), expected.begin() + high, [](float v) { return v != 0.f; });
    require(useful != 0, "initial reference PCM must be useful audio");

    std::thread::id firstWorker, lastWorker;
    std::uint64_t ticks = 0;
    auto fleet = create(rate, maximum, ES_RT_BALANCED);
    expectStopped(*fleet);
    const auto token = prepare(*fleet, model, rate);
    auto &voice = *fleet->cells[token.cell].voice;
    RuntimeTestAccess::pull(voice.runtime).setTickCommands([&] {
        if (!ticks++) firstWorker = std::this_thread::get_id();
        lastWorker = std::this_thread::get_id();
    }, {});
    activate(*fleet, token);
    auto physical = event(2, change, ES_RT_VECTOR);
    physical.voice = {1, 1, 0};
    physical.physical = vector;
    require(es_rt_submit(fleet.get(), &physical) == ES_RT_OK, "queue physical vector");
    auto stop = event(3, detached, ES_RT_STOP);
    stop.voice = physical.voice;
    require(es_rt_submit(fleet.get(), &stop) == ES_RT_OK, "queue immediate detach");

    start(*fleet);
    require(es_rt_start(fleet.get(), nullptr) == ES_RT_STATE, "start must be one-shot");
    auto invalid = event(4, 12000, ES_RT_MASTER);
    invalid.gain = .75;
    invalid.duration_ms = 101;
    require(es_rt_submit(fleet.get(), &invalid) == ES_RT_INVALID, "invalid master duration was admitted");
    const auto primed = observe(*fleet, "primed");
    require(primed.submitted == 0 && primed.generated == high && primed.sealed == high &&
            primed.low_frames == high - maximum && primed.high_frames == high && primed.storage_frames == high + 128,
            "startup did not retain exactly H frames");
    audioThread([&] {
        std::this_thread::sleep_for(5ms);
        require(observe(*fleet, "unconsumed").generated == high, "producer advanced without callback credit");
        consume(*fleet, expected, 0, 63);
        consume(*fleet, expected, 63, maximum - 64);
        std::this_thread::sleep_for(5ms);
        const auto upper = observe(*fleet, "above-low");
        require(upper.submitted == maximum - 1 && upper.generated == high,
                "producer refilled above the low-water threshold");
        consume(*fleet, expected, maximum - 1, 1);
        await([&] { return transport(*fleet).generated == maximum + high; }, "producer did not refill at low water");
        const auto refilled = observe(*fleet, "refilled");
        require(refilled.submitted == maximum && refilled.generated == refilled.submitted + refilled.high_frames &&
                refilled.sealed == refilled.generated, "refill did not reach exact high-water credit");
        consume(*fleet, expected, maximum, 65);
        std::this_thread::sleep_for(5ms);
        require(observe(*fleet, "partial").generated == maximum + high, "partial callback released excess credit");
        for (unsigned first = maximum + 65; first < end;) {
            const auto count = std::min(777u, end - first);
            await([&] { return transport(*fleet).generated >= first + count; }, "running refills stalled");
            consume(*fleet, expected, first, count);
            first += count;
        }
        await([&] {
            const auto state = transport(*fleet);
            return state.generated == state.sealed && state.generated > end + state.low_frames;
        }, "producer did not refill within the rounded credit");
        const auto last = observe(*fleet, "last-callback");
        require(last.generated - last.submitted <= last.high_frames, "rounded credit bound");
    });
    std::this_thread::sleep_for(5ms);
    const auto beforeJoin = observe(*fleet, "host-stopped");
    require(es_rt_quiesce(fleet.get()) == ES_RT_OK, "join without any more callback progress");
    const auto joined = observe(*fleet, "joined");
    require(beforeJoin.generated == joined.generated && joined.returned_cells == 1, "off-thread join advanced a full ring");
    require(es_rt_quiesce(fleet.get()) == ES_RT_OK && es_rt_start(fleet.get(), nullptr) == ES_RT_STATE,
            "quiescence is terminal and idempotent");
    expectStopped(*fleet);
    checkResult(*fleet, 1, 0);
    checkResult(*fleet, 2, change);
    checkResult(*fleet, 3, detached);
    require(firstWorker == lastWorker && firstWorker != std::this_thread::get_id() &&
            voice.runtime.telemetry().frames == detached && ticks == detached, "single worker or exact immediate detach");
    std::cout << "W thread=" << firstWorker << " first-tick=0 final-ticks=" << ticks << " Fs=" << rate
              << " raw-primed-frames=" << high << " primed-nonzero=" << useful
              << " bitwise-verified-frames=" << end << '\n';
}

void fault(const fs::path &model) {
    constexpr unsigned rate = 44100, prefix = 3905, high = 4096;
    auto reference = raw(model, rate);
    rawControls(*reference, initial);
    std::vector<float> expected(high);
    renderRaw(*reference, expected.data(), prefix);
    require(std::any_of(expected.begin(), expected.begin() + prefix, [](float v) { return v != 0.f; }),
            "fault prefix must retain useful audio");
    const auto last = static_cast<double>(expected[prefix - 1]);
    for (unsigned i = 0; i < 31; ++i) expected[prefix + i] = static_cast<float>(last * (31 - i) / 32.);
    es_telemetry rawState{};
    rawState.size = sizeof(rawState);
    rawState.abi = ES_ABI_V2;
    es_error problem{};
    require(es_get_telemetry(reference.get(), &rawState, &problem) == ES_OK, problem.message);

    unsigned visits = 0;
    std::thread::id producerThread;
    auto fleet = create(rate, 1024, ES_RT_BALANCED);
    const auto token = prepare(*fleet, model, rate);
    auto &voice = *fleet->cells[token.cell].voice;
    auto &shaft = RuntimeTestAccess::simulator(voice.runtime).getEngine()->getOutputCrankshaft()->m_body;
    RuntimeTestAccess::pull(voice.runtime).setTickCommands([&] {
        if (visits == 0) producerThread = std::this_thread::get_id();
        if (visits++ == prefix) shaft.theta = std::numeric_limits<double>::infinity();
    }, {});
    activate(*fleet, token);
    start(*fleet);
    require(observe(*fleet, "fault-primed").generated == high, "one voice fault prevented priming");
    audioThread([&] {
        for (unsigned i = 0; i < high; i += 1024) consume(*fleet, expected, i, 1024);
    });
    require(es_rt_quiesce(fleet.get()) == ES_RT_OK, "join producer with failed voice");
    auto actual = voice.runtime.telemetry();
    require(actual.state == ES_FAULTED && actual.frames == prefix && visits == prefix + 1 &&
            voice.runtime.processingFailure() == engine_sim::ProcessingStatus::mechanical_state,
            "failed voice did not preserve exact prefix or retried fault");
    actual.state = ES_READY;
    require(std::memcmp(&actual, &rawState, sizeof(actual)) == 0, "fault last-success telemetry");
    std::cout << "W thread=" << producerThread << " first-tick=0 fault-frame=" << prefix <<
        " pcm-prefix=" << prefix << " fault-tail-frames=32 callbacks-report-fallback=0\n";
}

void breakInFlight(const fs::path &model) {
    constexpr unsigned rate = 44100, high = 192;
    auto reference = raw(model, rate);
    rawControls(*reference, initial);
    std::vector<float> expected(high);
    renderRaw(*reference, expected.data(), expected.size());

    std::atomic<bool> parked{false}, allow{false};
    unsigned visits = 0;
    auto fleet = create(rate, 64, ES_RT_INTERACTIVE);
    const auto token = prepare(*fleet, model, rate);
    auto &voice = *fleet->cells[token.cell].voice;
    RuntimeTestAccess::pull(voice.runtime).setTickCommands([&] {
        if (visits++ != high) return;
        parked.store(true, std::memory_order_release);
        while (!allow.load(std::memory_order_acquire)) std::this_thread::yield();
    }, {});
    activate(*fleet, token);
    start(*fleet);
    try {
        audioThread([&] {
            consume(*fleet, expected, 0, 64);
            await([&] { return parked.load(std::memory_order_acquire); }, "producer never reached in-flight tick");
            consume(*fleet, expected, 64, 64);
            consume(*fleet, expected, 128, 64);
            consume(*fleet, expected, high, 64, true);
            const auto broken = observe(*fleet, "in-flight-break");
            require(broken.broken && broken.generated == high && broken.sealed == high + 64, "break timeline");
            allow.store(true, std::memory_order_release);
            consume(*fleet, expected, high + 64, 64, true);
        });
    } catch (...) {
        allow.store(true, std::memory_order_release);
        es_rt_quiesce(fleet.get());
        throw;
    }
    require(es_rt_quiesce(fleet.get()) == ES_RT_OK, "join in-flight producer");
    const auto joined = observe(*fleet, "break-joined");
    require(joined.generated == high && joined.submitted == high + 128 &&
            visits == high + 64, "released in-flight tile published stale PCM or generated more work");
}

void startupChecks() {
    auto shutdown = create(44100, 64, ES_RT_INTERACTIVE);
    require(es_rt_quiesce(shutdown.get()) == ES_RT_OK && es_rt_start(shutdown.get(), nullptr) == ES_RT_STATE,
            "quiesce before start");
}
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    try {
        const auto input = fs::absolute(argv[2]);
        const std::string_view scenario = argv[1];
        if (scenario == "horizon") {
            startupChecks();
            horizon(input, 44100);
            horizon(input, 48000);
        } else if (scenario == "fault") {
            fault(input);
            breakInFlight(input);
        } else {
            return 2;
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
