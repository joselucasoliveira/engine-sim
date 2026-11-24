#include "owner.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace engine_sim {
struct RuntimeTestAccess {
    static PullEngine &pull(Runtime &runtime) { return *runtime.m_session->pull; }
    static Simulator &simulator(Runtime &runtime) { return *runtime.m_session->prepared.session->simulator; }
};
}

namespace {
namespace fs = std::filesystem;
namespace core = engine_sim::fleet;
using engine_sim::RuntimeTestAccess;
using namespace std::chrono_literals;
using Fleet = std::unique_ptr<es_rt_fleet, decltype(&es_rt_destroy)>;
constexpr unsigned rate = 44100;
constexpr std::uint64_t epoch = 218;
constexpr unsigned quantum = 64;
constexpr es_rt_vector initial{.25, 0, 0, 1, 1};

void require(bool condition, const char *reason) {
    if (!condition) throw std::runtime_error(reason);
}

template<class Condition>
void waitFor(Condition condition, const char *reason) {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (!condition()) {
        require(std::chrono::steady_clock::now() < deadline, reason);
        std::this_thread::sleep_for(100us);
    }
}

Fleet create(unsigned lanes = 1, unsigned maximum = 128) {
    es_rt_config config{sizeof(config), ES_RT_V1, rate, ES_RT_BALANCED, maximum, lanes, 0, 0, epoch};
    es_rt_fleet *result = nullptr;
    require(es_rt_create(&config, &result, nullptr) == ES_RT_OK && result, "create fleet observer");
    return {result, es_rt_destroy};
}

std::int32_t prepare(es_rt_fleet &fleet, const fs::path &model, es_rt_token &token) {
    const std::string file = model.u8string(), assets = model.parent_path().u8string();
    const es_rt_voice_config config{sizeof(config), ES_RT_V1, ES_RT_FREE, rate, ES_RT_RAW, 0, 403, 0, initial};
    return es_rt_prepare_file(&fleet, &config, file.c_str(), assets.c_str(), &token, nullptr);
}

es_rt_event event(unsigned kind, std::uint64_t sequence, std::uint64_t frame) {
    es_rt_event value{};
    value.size = sizeof(value);
    value.abi = ES_RT_V1;
    value.kind = kind;
    value.epoch = epoch;
    value.sequence = sequence;
    value.frame = frame;
    return value;
}

void submit(es_rt_fleet &fleet, const es_rt_event &command, std::int32_t expected = ES_RT_OK) {
    const auto status = es_rt_submit(&fleet, &command);
    if (status != expected) {
        std::cerr << "sequence=" << command.sequence << " frame=" << command.frame
                  << " kind=" << command.kind << " expected=" << expected << " status=" << status << '\n';
    }
    require(status == expected, "event disposition");
}

void activate(es_rt_fleet &fleet, es_rt_token token, std::uint64_t generation,
              std::uint64_t sequence, std::uint64_t frame) {
    auto command = event(ES_RT_ACTIVATE, sequence, frame);
    command.candidate = token;
    command.next_voice = {generation, 0, 0};
    submit(fleet, command);
}

void start(es_rt_fleet &fleet) { require(es_rt_start(&fleet, nullptr) == ES_RT_OK, "start telemetry fleet"); }
void quiesce(es_rt_fleet &fleet) { require(es_rt_quiesce(&fleet) == ES_RT_OK, "quiesce telemetry fleet"); }

es_rt_transport readTransport(es_rt_fleet &fleet) {
    es_rt_transport value{};
    value.size = sizeof(value);
    value.abi = ES_RT_V1;
    require(es_rt_get_transport(&fleet, &value) == ES_RT_OK && value.epoch == epoch, "read transport observables");
    return value;
}

std::int32_t poll(es_rt_fleet &fleet, es_rt_snapshot &snapshot) {
    snapshot.size = sizeof(snapshot);
    snapshot.abi = ES_RT_V1;
    return es_rt_poll_snapshot(&fleet, &snapshot);
}

es_rt_result receipt(es_rt_fleet &fleet, std::uint64_t sequence, std::uint64_t frame,
                     std::uint32_t reason = ES_RT_OK) {
    es_rt_result result{};
    result.size = sizeof(result);
    result.abi = ES_RT_V1;
    require(es_rt_poll_result(&fleet, sequence, &result) == ES_RT_OK && result.sequence == sequence &&
            result.epoch == epoch && result.requested == frame && result.reason == reason &&
            result.applied == (reason == ES_RT_OK ? frame : ES_RT_NO_FRAME) &&
            result.ended == (reason == ES_RT_OK ? frame : ES_RT_NO_FRAME), "terminal receipt timestamps");
    return result;
}

class Audio {
public:
    explicit Audio(es_rt_fleet &owner) : fleet(owner), worker(&Audio::run, this) {}
    ~Audio() { stop(); }

    void allow(std::uint64_t frame) {
        target.store(frame, std::memory_order_release);
    }
    void wait(std::uint64_t frame) {
        waitFor([&] { return done.load(std::memory_order_acquire) >= frame || failed.load(); },
                "callback timed out waiting for production");
        require(!failed.load(), "callback lost available PCM");
    }
    void through(std::uint64_t frame) {
        allow(frame);
        wait(frame);
    }
    void stop() {
        stopping.store(true, std::memory_order_release);
        if (worker.joinable()) worker.join();
    }

private:
    void run() noexcept {
        float samples[quantum]{};
        es_rt_audio audio{sizeof(audio), ES_RT_V1, quantum, quantum, samples, {}};
        std::uint64_t first = 0;
        while (!stopping.load(std::memory_order_acquire)) {
            const auto end = first + quantum;
            // The host-side test pacing observes G outside es_rt_process.
            if (end > target.load(std::memory_order_acquire) ||
                end > fleet.producer.generated.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(100us);
                continue;
            }
            es_rt_block block{sizeof(block), ES_RT_V1, 0, 0, 0, 0};
            if (es_rt_process(&fleet, &audio, &block) != ES_RT_OK || block.epoch != epoch ||
                    block.first != first || block.pcm_frames != quantum || block.fallback_frames != 0) {
                failed.store(true, std::memory_order_release);
                return;
            }
            first = end;
            done.store(first, std::memory_order_release);
        }
    }

    es_rt_fleet &fleet;
    std::atomic<std::uint64_t> target{0}, done{0};
    std::atomic<bool> stopping{false}, failed{false};
    std::thread worker;
};

constexpr std::uint64_t changeInterval = 67, changes = 64;
es_rt_vector targetFor(std::uint64_t index) {
    if (!index) return initial;
    return {static_cast<double>(index % 11) / 10., static_cast<double>(index % 7) / 8., 0., 1, 1};
}

void checkVector(const es_rt_vector &value, const es_rt_vector &expected) {
    require(value.demand == expected.demand && value.load_nm == expected.load_nm &&
        value.driven_rpm == expected.driven_rpm && value.ignition == expected.ignition &&
        value.starter == expected.starter, "torn physical vector");
}

void checkSnapshot(const es_rt_snapshot &snapshot) {
    require(snapshot.epoch == epoch && snapshot.lanes == 2 && snapshot.reserved == 0 &&
            snapshot.generated != 0 && snapshot.generated % quantum == 0, "snapshot identity and age");
    const auto &lane = snapshot.lane[0];
    const auto change = std::min(changes, (snapshot.generated - 1) / changeInterval);
    require(lane.voice.generation == 1 && lane.voice.lane == 0 && lane.state == ES_RT_PLAY && lane.fault == ES_RT_OK &&
            lane.gain == 1. && lane.gate == 1. && lane.frames == snapshot.generated &&
            lane.ticks == lane.frames && lane.native_frames == lane.frames && lane.last_sequence == change + 1 &&
            lane.last_latched_sequence == change + 1 && lane.requested_frame == change * changeInterval &&
            lane.latch_frame == lane.requested_frame && lane.latch_tick == lane.latch_frame &&
            std::isfinite(lane.measured_rpm) && std::isfinite(lane.cycle_rad) &&
            std::isfinite(lane.load_reaction_nm) && std::isfinite(lane.plate_angle_rad), "coherent voice fields");
    checkVector(lane.requested, targetFor(change));
    checkVector(lane.applied, targetFor(change));
    require(snapshot.lane[1].state == ES_RT_VACANT && snapshot.lane[1].latch_frame == ES_RT_NO_FRAME &&
            snapshot.lane[1].voice.generation == 0 && snapshot.lane[1].frames == 0, "vacant lane leaked state");
}

void telemetry(const fs::path &model) {
    auto fleet = create(2);
    es_rt_snapshot snapshot{};
    snapshot.epoch = 23;
    require(poll(*fleet, snapshot) == ES_RT_EMPTY && snapshot.epoch == 23, "empty snapshot modified destination");
    --snapshot.size;
    require(es_rt_poll_snapshot(fleet.get(), &snapshot) == ES_RT_CAPACITY && snapshot.epoch == 23,
            "undersized snapshot modified destination");
    es_rt_token token{};
    require(prepare(*fleet, model, token) == ES_RT_OK, "prepare observed voice");
    activate(*fleet, token, 1, 1, 0);
    for (std::uint64_t i = 1; i <= changes; ++i) {
        auto command = event(ES_RT_VECTOR, i + 1, i * changeInterval);
        command.voice = {1, 0, 0};
        command.physical = targetFor(i);
        submit(*fleet, command);
    }
    start(*fleet);
    Audio audio(*fleet);
    audio.through(3072);
    const auto stalled = readTransport(*fleet);
    require(stalled.submitted == 3072 && stalled.generated > 1024 && stalled.dropped_snapshots >= 32 && !stalled.broken,
            "stalled snapshot consumer blocked PCM or failed to count drops");
    for (unsigned i = 1; i <= 16; ++i) {
        require(poll(*fleet, snapshot) == ES_RT_OK && snapshot.generated == i * quantum && snapshot.dropped == 0,
                "full snapshot queue overwrote its oldest record");
        checkSnapshot(snapshot);
    }
    std::uint64_t lastFrame = 16 * quantum, lastDrops = 0;
    unsigned observed = 16;
    for (std::uint64_t next = 3200; next <= 12288; next += 128) {
        audio.allow(next);
        bool ready = false;
        waitFor([&] {
            if (poll(*fleet, snapshot) == ES_RT_OK) {
                checkSnapshot(snapshot);
                require(snapshot.generated > lastFrame && snapshot.dropped >= lastDrops, "telemetry age regressed");
                lastFrame = snapshot.generated;
                lastDrops = snapshot.dropped;
                ++observed;
                ready = snapshot.generated >= next;
            }
            return ready;
        }, "snapshot producer stalled after reader resumed");
        audio.wait(next);
    }
    const auto reopened = readTransport(*fleet);
    require(lastDrops >= stalled.dropped_snapshots && reopened.dropped_snapshots >= lastDrops &&
            observed >= 16 + 64, "snapshot ring reuse or loss report");
    audio.stop();
    quiesce(*fleet);
    while (poll(*fleet, snapshot) == ES_RT_OK) {
        checkSnapshot(snapshot);
        require(snapshot.generated > lastFrame, "post-join snapshot moved backwards");
        lastFrame = snapshot.generated;
    }
    receipt(*fleet, 1, 0);
    for (std::uint64_t i = 1; i <= changes; ++i) receipt(*fleet, i + 1, i * changeInterval);
    es_rt_lane_info final{};
    es_rt_token retired{};
    require(es_rt_collect(fleet.get(), &retired, &final) == ES_RT_OK && retired.generation == token.generation &&
            final.voice.generation == 1 && final.frames >= lastFrame, "quiesce did not return observed voice");
    std::cout << "stalled D=" << stalled.submitted << " G=" << stalled.generated
              << " drops=" << stalled.dropped_snapshots << " queue high=16, observed=" << observed
              << " last-observed=" << lastFrame << " final-voice-frames=" << final.frames << '\n';
}

struct LifeCount {
    const std::thread::id coordinator = std::this_thread::get_id();
    unsigned created = 0;
    std::atomic<unsigned> destroyed{0};
    std::atomic<unsigned> wrongThread{0};
};

struct Witness {
    explicit Witness(LifeCount &count) : life(count) { ++life.created; }
    ~Witness() {
        ++life.destroyed;
        if (std::this_thread::get_id() != life.coordinator) ++life.wrongThread;
    }
    LifeCount &life;
};

std::weak_ptr<Witness> witness(es_rt_fleet &fleet, es_rt_token token, LifeCount &life, bool fault = false) {
    auto &voice = *fleet.cells[token.cell].voice;
    auto alive = std::make_shared<Witness>(life);
    auto &shaft = RuntimeTestAccess::simulator(voice.runtime).getEngine()->getOutputCrankshaft()->m_body;
    RuntimeTestAccess::pull(voice.runtime).setTickCommands([alive, &shaft, fault, at = 0u]() mutable {
        if (fault && at++ == 81) shaft.theta = std::numeric_limits<double>::infinity();
    }, {});
    return alive;
}

es_rt_token collect(es_rt_fleet &fleet, es_rt_lane_info &info) {
    es_rt_token token{};
    require(es_rt_collect(&fleet, &token, &info) == ES_RT_OK && token.epoch == epoch, "collect returned cell");
    require(es_rt_discard(&fleet, token) == ES_RT_STALE, "historical token became a prepared token");
    return token;
}

void backlog(const fs::path &model, LifeCount &life) {
    auto fleet = create();
    es_rt_token pending{};
    require(prepare(*fleet, model, pending) == ES_RT_OK, "prepare discard witness");
    const auto untransferred = witness(*fleet, pending, life);
    require(es_rt_discard(fleet.get(), pending) == ES_RT_OK && untransferred.expired(), "discard lost C ownership");
    start(*fleet);
    Audio audio(*fleet);
    std::uint64_t ran = 0;
    std::array<es_rt_token, 4> tokens{};
    std::array<std::weak_ptr<Witness>, 4> lives{};
    for (std::size_t i = 0; i < lives.size(); ++i) {
        auto &token = tokens[i];
        require(prepare(*fleet, model, token) == ES_RT_OK, "prepare another retiree");
        lives[i] = witness(*fleet, token, life, i == 0);
        const auto position = readTransport(*fleet);
        const auto first = position.submitted + position.high_frames + 2 * quantum;
        const auto last = first + 137;
        activate(*fleet, token, i + 1, 2 * i + 1, first);
        auto stop = event(ES_RT_STOP, 2 * i + 2, last);
        stop.voice = {i + 1, 0, 0};
        submit(*fleet, stop);
        ran = (last + quantum - 1) / quantum * quantum;
        audio.through(ran);
        receipt(*fleet, stop.sequence - 1, first);
        if (i < 3) receipt(*fleet, stop.sequence, last);
        require(!lives[i].expired(), "producer destroyed returned cell");
    }
    const auto full = readTransport(*fleet);
    require(full.returned_cells == 4 && full.outstanding == 1, "retirement backlog is not bounded by fixed cells");
    es_rt_token unavailable{9, 8, 7, 0};
    require(prepare(*fleet, model, unavailable) == ES_RT_CAPACITY && unavailable.epoch == 9,
            "full return backlog overwrote a voice");
    es_rt_lane_info info{};
    auto first = collect(*fleet, info);
    require(first.cell == tokens[0].cell && info.voice.generation == 1 && info.state == ES_RT_FAILED &&
            info.fault == ES_RT_FAULT && info.frames == 81 && info.ticks == 81 && info.native_frames == 81 &&
            info.last_sequence == 2 && lives[0].expired(), "failed voice final state or ownership");
    for (unsigned i = 1; i < tokens.size(); ++i) {
        auto taken = collect(*fleet, info);
        require(taken.cell == tokens[i].cell && info.voice.generation == i + 1 && info.frames == 137 &&
                info.ticks == 137 && info.last_sequence == 2 * i + 2 && lives[i].expired(), "detached voice final state");
    }
    auto numeric = fleet->commands.lastFrame;
    es_rt_token reused{};
    std::array<std::weak_ptr<Witness>, 4> reusedLives{};
    for (unsigned i = 0; i < reusedLives.size(); ++i) {
        const auto position = readTransport(*fleet);
        const auto frame = position.submitted + position.high_frames + 2 * quantum;
        require(prepare(*fleet, model, reused) == ES_RT_OK && reused.cell == tokens[0].cell &&
            reused.generation == tokens[0].generation + i + 1, "physical generation reuse after collection");
        reusedLives[i] = witness(*fleet, reused, life);
        if (i < 2) {
            const auto oldId = 4 + i;
            // Before the older receipt is reaped, its logical identity still
            // points to a former cell. It may not control this candidate.
            auto stale = event(ES_RT_VECTOR, 9 + 2 * i, frame);
            stale.voice = {oldId, 0, 0};
            submit(*fleet, stale, ES_RT_STALE);
            receipt(*fleet, 8 + 2 * i, numeric);
        }
        activate(*fleet, reused, 5 + i, 9 + 2 * i, frame);
        const auto end = frame + 97;
        auto stop = event(ES_RT_STOP, 10 + 2 * i, end);
        stop.voice = {5 + i, 0, 0};
        submit(*fleet, stop);
        audio.through((end + quantum - 1) / quantum * quantum);
        receipt(*fleet, stop.sequence - 1, frame);
        if (i != 0) receipt(*fleet, stop.sequence, end);
        numeric = end;
        collect(*fleet, info);
        require(info.voice.generation == 5 + i && info.frames == 97 && reusedLives[i].expired(),
                "recycled physical storage leaked a previous engine identity");
    }
    audio.stop();
    quiesce(*fleet);
    es_rt_token untouched{8, 7, 6, 0};
    info.frames = 99;
    require(es_rt_collect(fleet.get(), &untouched, &info) == ES_RT_EMPTY && untouched.epoch == 8 && info.frames == 99,
            "empty return scan changed output storage");
    std::cout << "backlog D=" << full.submitted << " retired=" << full.returned_cells
              << " failed-prefix=81 detached=3 reused=4\n";
}

void queuedAndStopped(const fs::path &model, LifeCount &life) {
    {
        auto fleet = create();
        es_rt_token active{}, unused{};
        require(prepare(*fleet, model, active) == ES_RT_OK && prepare(*fleet, model, unused) == ES_RT_OK, "prepare shutdown tokens");
        const auto transferred = witness(*fleet, active, life);
        const auto retained = witness(*fleet, unused, life);
        activate(*fleet, active, 1, 1, 0);
        auto command = event(ES_RT_VECTOR, 2, 1024);
        command.voice = {1, 0, 0};
        for (std::uint64_t i = 2; i <= 248; ++i) {
            command.sequence = i;
            command.frame = 1024 + i * quantum;
            submit(*fleet, command);
        }
        command.sequence = 249;
        command.frame += quantum;
        submit(*fleet, command, ES_RT_FULL);
        require(readTransport(*fleet).queue_rejections == 1, "queue rejections count");
        quiesce(*fleet);
        quiesce(*fleet);
        // The canceled activation returns its cell even with pending receipts.
        es_rt_lane_info final{};
        auto returned = collect(*fleet, final);
        require(returned.cell == active.cell && transferred.expired() && final.frames == 0 &&
                final.voice.generation == 1 && final.latch_frame == ES_RT_NO_FRAME, "unstarted activation cancellation");
        require(es_rt_discard(fleet.get(), unused) == ES_RT_OK && retained.expired(), "untransferred token remains C-owned");
        receipt(*fleet, 1, 0, ES_RT_CANCELED);
        for (std::uint64_t i = 2; i <= 248; ++i) receipt(*fleet, i, 1024 + i * quantum, ES_RT_CANCELED);
        require(readTransport(*fleet).outstanding == 0, "canceled tickets stranded");
        std::cout << "before-start canceled-tickets=248 synchronous-full=1\n";
    }
    {
        auto fleet = create();
        es_rt_token active{};
        require(prepare(*fleet, model, active) == ES_RT_OK, "prepare primed shutdown");
        auto lifeToken = witness(*fleet, active, life);
        activate(*fleet, active, 1, 1, 0);
        auto command = event(ES_RT_STOP, 2, 4096);
        command.voice = {1, 0, 0};
        submit(*fleet, command);
        start(*fleet);
        quiesce(*fleet);
        const auto finalTransport = readTransport(*fleet);
        require(finalTransport.submitted == 0 && finalTransport.generated == 512 &&
                finalTransport.returned_cells == 1, "stopped-before-device lost final cursor or active cell");
        receipt(*fleet, 2, 4096, ES_RT_CANCELED);
        receipt(*fleet, 1, 0);
        es_rt_lane_info final{};
        collect(*fleet, final);
        require(final.frames == 512 && lifeToken.expired(), "active return did not copy last completed state");
        std::cout << "pre-device discarded=" << finalTransport.generated - finalTransport.submitted
                  << " active-returned=1 canceled=1\n";
    }
}

void retirement(const fs::path &model) {
    LifeCount life;
    backlog(model, life);
    queuedAndStopped(model, life);
    require(life.created == 12 && life.destroyed.load() == life.created && life.wrongThread.load() == 0,
            "voice lifetime counters");
    std::cout << "live-initial=0 constructed=" << life.created << " destroyed=" << life.destroyed.load()
              << " live-final=0 noncoordinator-deletions=" << life.wrongThread.load() << '\n';
}
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    try {
        const auto model = fs::absolute(argv[2]);
        const std::string_view mode = argv[1];
        if (mode == "telemetry") telemetry(model);
        else if (mode == "retirement") retirement(model);
        else return 2;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
