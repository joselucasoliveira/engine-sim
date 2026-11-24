#include "../fleet-process/support.hpp"

#ifdef ES_R3_EPOCH_OBSERVER
#include "escape.hpp"
#include "observe.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace engine_sim {
struct RuntimeTestAccess {
    static PullEngine &pull(Runtime &runtime) { return *runtime.m_session->pull; }
};
namespace fleet {
struct PcmTransportTestAccess {
    static void cursors(PcmTransport &pcm, std::uint64_t read, std::uint64_t write) {
        pcm.m_read.store(read, std::memory_order_relaxed);
        pcm.m_write.store(write, std::memory_order_release);
    }
    static void corruptEpoch(PcmTransport &pcm) {
        const auto read = pcm.m_read.load(std::memory_order_relaxed);
        ++pcm.m_slots[read % pcm.m_count].epoch;
    }
    static std::uint64_t read(const PcmTransport &pcm) { return pcm.m_read.load(std::memory_order_acquire); }
};
}
}

namespace {
namespace fs = std::filesystem;
namespace core = engine_sim::fleet;
using namespace std::chrono_literals;
using fleet_process_test::require;
using core::PcmTransportTestAccess;
constexpr unsigned q = core::tileFrames;
constexpr unsigned all = 0x1ff;
constexpr std::uint64_t epochBase = 715;
constexpr es_rt_vector initial{.25, 0, 0, 1, 1};

std::int32_t guarded(es_rt_fleet &fleet, const es_rt_audio &audio, es_rt_block &result, bool control = false) {
#ifdef ES_R3_EPOCH_OBSERVER
    realtime_probe::Record hits{};
    std::int32_t status = ES_RT_INTERNAL;
    {
        const realtime_probe::Scope active(hits);
        if (control) {
            void *memory = std::malloc(64);
            escape(memory);
            std::free(memory);
        }
        status = es_rt_process(&fleet, &audio, &result);
    }
    for (std::size_t i = 0; i < hits.size(); ++i) {
        const bool expected = control && (i == static_cast<std::size_t>(realtime_probe::Operation::malloc) ||
                                          i == static_cast<std::size_t>(realtime_probe::Operation::free));
        require(hits[i].count == (expected ? 1u : 0u), realtime_probe::names[i]);
    }
    if (control) realtime_probe::dump(hits);
    return status;
#else
    (void)control;
    return es_rt_process(&fleet, &audio, &result);
#endif
}

es_rt_transport transport(es_rt_fleet &fleet) {
    es_rt_transport state{};
    state.size = sizeof(state);
    state.abi = ES_RT_V1;
    require(es_rt_get_transport(&fleet, &state) == ES_RT_OK, "get epoch transport");
    return state;
}

void allSpans() {
    unsigned spans = 0;
    for (auto profile : {ES_RT_INTERACTIVE, ES_RT_BALANCED}) {
        for (unsigned maximum : {64u, 128u, 256u, 512u, 1024u}) {
            if (profile == ES_RT_INTERACTIVE && maximum > 128) continue;
            auto owner = fleet_process_test::create(maximum, 8, profile);
            std::uint64_t delivered = 0, produced = 0;
            for (unsigned n = 0; n <= maximum; ++n) {
                while (produced < delivered + n) {
                    require(owner->pcm.tryPublish(fleet_process_test::tile(produced)), "independent transport credit");
                    produced += q;
                }
                fleet_process_test::Call sample(n, n ? all : 0);
                sample.check(guarded(*owner, sample.audio, sample.block), delivered, n);
                delivered += n;
                require(transport(*owner).submitted == delivered, "span did not advance exact D");
                ++spans;
            }
            fleet_process_test::Call invalid(maximum + 1, all);
            invalid.rejected(owner.get(), ES_RT_CAPACITY, &invalid.audio, &invalid.block);
        }
    }
    std::cout << "valid frame extents=" << spans << " across all seven M/profile combinations\n";
}

void stalePartial() {
    auto owner = fleet_process_test::create(128, 8);
    require(owner->pcm.tryPublish(fleet_process_test::tile(0)), "publish first for partial");
    fleet_process_test::Call first(13, all);
    first.check(guarded(*owner, first.audio, first.block), 0, 13);
    require(PcmTransportTestAccess::read(owner->pcm) == 0, "partial slot released early");

    // The deterministic driver owns both sides here and corrupts the borrowed tag.
    PcmTransportTestAccess::corruptEpoch(owner->pcm);
    require(owner->pcm.tryPublish(fleet_process_test::tile(64)), "publish valid tile after the stale tag");
    fleet_process_test::Call empty(0, 0);
    empty.check(guarded(*owner, empty.audio, empty.block), 13, 0);
    require(!transport(*owner).broken, "zero saw stale tag");
    fleet_process_test::Call stale(99, all);
    stale.check(guarded(*owner, stale.audio, stale.block), 13, 0);
    require(PcmTransportTestAccess::read(owner->pcm) == 0, "invalid tag released borrowed slot");
    fleet_process_test::Call again(1, all);
    again.check(guarded(*owner, again.audio, again.block), 112, 0);
    require(transport(*owner).broken, "stale partial slot did not break the epoch");
    std::cout << "partially consumed stale tag D=13 pcm=0 filler=99 next-filler=1\n";
}

void corruptCredit() {
    for (const auto write : {4u, 12u}) {
        auto fleet = fleet_process_test::create(64, 8);
        PcmTransportTestAccess::cursors(fleet->pcm, 5, write);
        auto tile = fleet_process_test::tile(0);
        require(!fleet->pcm.tryPublish(tile), "invalid producer distance was granted credit");
        fleet_process_test::Call block(64, all);
        block.check(guarded(*fleet, block.audio, block.block), 0, 0);
        require(transport(*fleet).submitted == 64 && transport(*fleet).broken &&
            PcmTransportTestAccess::read(fleet->pcm) == 5, "index corruption consumed an inaccessible tile");
    }
}

void nearWrap() {
    auto fleet = fleet_process_test::create(64, 8);
    constexpr auto top = UINT64_MAX;
    const auto first = top - 127;
    PcmTransportTestAccess::cursors(fleet->pcm, top - 1, top - 1);
    fleet->commands.submitted.store(first);
    require(fleet->pcm.tryPublish(fleet_process_test::tile(first)), "last publishable index");
    require(!fleet->pcm.tryPublish(fleet_process_test::tile(first + q)), "write index wrapped");
    fleet_process_test::Call good(q, all);
    good.check(guarded(*fleet, good.audio, good.block), first, q);
    require(PcmTransportTestAccess::read(fleet->pcm) == top, "read index wrapped");
    fleet_process_test::Call overflow(q, all);
    overflow.rejected(fleet.get(), ES_RT_CAPACITY, &overflow.audio, &overflow.block);
    require(!transport(*fleet).broken, "capacity rejection marked starvation");
    fleet_process_test::Call fallback(q - 1, all);
    fallback.check(guarded(*fleet, fallback.audio, fallback.block), first + q, 0);
    require(transport(*fleet).submitted == top && transport(*fleet).broken, "last valid output count");
    fleet_process_test::Call tooLate(1, all);
    tooLate.rejected(fleet.get(), ES_RT_CAPACITY, &tooLate.audio, &tooLate.block);
    std::cout << "near-wrap valid D=" << transport(*fleet).submitted << " published/reader index=" << top << '\n';
}

template<class Condition>
void await(Condition condition, const char *message) {
    const auto deadline = std::chrono::steady_clock::now() + 6s;
    while (!condition()) {
        require(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::yield();
    }
}

using Owner = std::unique_ptr<es_rt_fleet, decltype(&es_rt_destroy)>;
Owner create(std::uint64_t epoch, unsigned maximum = 64) {
    const es_rt_config setup{sizeof(setup), ES_RT_V1, 44100, ES_RT_BALANCED, maximum, 1, ES_RT_EXPERIMENTAL, 0, epoch};
    es_rt_fleet *fleet = nullptr;
    require(es_rt_create(&setup, &fleet, nullptr) == ES_RT_OK && fleet, "create fresh epoch");
    return {fleet, es_rt_destroy};
}
std::int32_t prepare(es_rt_fleet &fleet, const fs::path &model, es_rt_token &out) {
    const auto path = model.u8string(), root = model.parent_path().u8string();
    const es_rt_voice_config voice{sizeof(voice), ES_RT_V1, ES_RT_FREE, 44100, ES_RT_RAW, 0, 403, 0, initial};
    return es_rt_prepare_file(&fleet, &voice, path.c_str(), root.c_str(), &out, nullptr);
}
es_rt_token prepare(es_rt_fleet &fleet, const fs::path &model) {
    es_rt_token token{};
    require(prepare(fleet, model, token) == ES_RT_OK, "prepare recovery model");
    return token;
}
es_rt_event event(const es_rt_fleet &fleet, unsigned kind, unsigned seq, unsigned frame) {
    es_rt_event e{};
    e.size = sizeof(e); e.abi = ES_RT_V1; e.epoch = fleet.config.epoch; e.kind = kind;
    e.sequence = seq; e.frame = frame;
    return e;
}
es_rt_event activate(es_rt_fleet &fleet, es_rt_token token) {
    auto e = event(fleet, ES_RT_ACTIVATE, 1, 0);
    e.candidate = token;
    e.next_voice = {1, 0, 0};
    require(es_rt_submit(&fleet, &e) == ES_RT_OK, "queue recovered initial voice");
    return e;
}
es_rt_event swap(es_rt_fleet &fleet, es_rt_token token, unsigned frame, unsigned duration) {
    auto e = event(fleet, ES_RT_REPLACE, 2, frame);
    e.candidate = token;
    e.duration_ms = duration;
    e.voice = {1, 0, 0}; e.next_voice = {2, 0, 0};
    require(es_rt_submit(&fleet, &e) == ES_RT_OK, "queue recovery overlap");
    return e;
}

void receipt(es_rt_fleet &fleet, const es_rt_event &event, unsigned reason,
             std::uint64_t applied, std::uint64_t ended) {
    es_rt_result out{};
    out.size = sizeof(out); out.abi = ES_RT_V1;
    require(es_rt_poll_result(&fleet, event.sequence, &out) == ES_RT_OK && out.epoch == event.epoch &&
        out.requested == event.frame && out.applied == applied && out.ended == ended && out.reason == reason,
        "epoch recovery receipt");
}

struct DeviceBlock {
    es_rt_fleet &fleet;
    std::uint64_t next = 0;
    unsigned useful = 0;
    void process(unsigned n, unsigned pcm, bool control = false) {
        std::array<float, q + 2> mix, stem;
        mix.fill(-63.f); stem.fill(-64.f);
        es_rt_audio audio{sizeof(audio), ES_RT_V1, n, q, mix.data() + 1, {stem.data() + 1}};
        es_rt_block result{sizeof(result), ES_RT_V1, 0, 0, 0, 0};
        require(guarded(fleet, audio, result, control) == (pcm == n ? ES_RT_OK : ES_RT_BROKEN) &&
                result.epoch == fleet.config.epoch && result.first == next &&
                result.pcm_frames == pcm && result.fallback_frames == n - pcm, "native prefix or fallback span");
        for (unsigned i = 1; i <= n; ++i) {
            if (i <= pcm) {
                require(mix[i] != -63.f && stem[i] != -64.f && std::isfinite(stem[i]) &&
                        mix[i] == static_cast<float>(stem[i] * .5), "uninitialized producer PCM");
                if (stem[i] != 0.f) ++useful;
            } else {
                float zero = 0;
                require(std::memcmp(&mix[i], &zero, sizeof(zero)) == 0 &&
                        std::memcmp(&stem[i], &zero, sizeof(zero)) == 0, "fallback must be positive zero");
            }
        }
        require(mix.front() == -63.f && stem.front() == -64.f &&
                mix[n + 1] == -63.f && stem[n + 1] == -64.f, "outside host span");
        next += n;
    }
};

struct Seams {
    std::atomic<unsigned> acknowledged{0}, released{0};
    std::atomic<bool> parked{false}, unblock{false}, failed{false};

    void coordinatorWait(unsigned step) {
        await([&] { return acknowledged.load(std::memory_order_acquire) >= step || failed.load(); },
              "coordinator did not observe callback handoff");
        require(!failed.load(), "callback did not reach the required seam");
    }
    void audioWait(unsigned step) {
        acknowledged.store(step, std::memory_order_release);
        await([&] { return released.load(std::memory_order_acquire) >= step; }, "callback not released by coordinator");
    }
};

void collectAll(es_rt_fleet &fleet, unsigned expected) {
    es_rt_lane_info info{};
    es_rt_token old{};
    unsigned count = 0;
    while (es_rt_collect(&fleet, &old, &info) == ES_RT_OK) {
        require(old.epoch == fleet.config.epoch, "return identity crosses epochs");
        ++count;
    }
    require(count == expected && fleet.prepared == 0 && fleet.commands.outstanding == 0 &&
            !fleet.commands.overlap && fleet.retainedBytes == fleet.fixedBytes, "epoch still owns abandoned work");
}

void nativeBroken(const fs::path &model, std::uint64_t epoch, bool control) {
    Seams seams;
    auto fleet = create(epoch);
    const auto old = prepare(*fleet, model);
    auto &runtime = fleet->cells[old.cell].voice->runtime;
    engine_sim::RuntimeTestAccess::pull(runtime).setTickCommands([&seams, tick = 0u]() mutable {
        if (tick++ != 256) return;
        seams.parked.store(true, std::memory_order_release);
        while (!seams.unblock.load(std::memory_order_acquire)) std::this_thread::yield();
    }, {});
    const auto first = activate(*fleet, old);
    const auto second = swap(*fleet, prepare(*fleet, model), 63, 100);
    require(es_rt_start(fleet.get(), nullptr) == ES_RT_OK && transport(*fleet).generated == 256, "closed 256-frame prime");
    DeviceBlock device{*fleet};
    std::exception_ptr error;
    std::thread audio([&] {
        try {
            device.process(63, 63);
            seams.audioWait(1);
            device.process(1, 1);
            seams.audioWait(2);
            device.process(64, 64);
            device.process(64, 64);
            device.process(59, 59);
            device.process(32, 5);
            seams.audioWait(3);
            device.process(0, 0, control);
            device.process(64, 0);
            seams.acknowledged.store(4, std::memory_order_release);
        } catch (...) {
            error = std::current_exception();
            seams.failed.store(true, std::memory_order_release);
        }
    });
    auto future = event(*fleet, ES_RT_VECTOR, 3, 1024);
    future.voice = {2, 0, 0};
    future.physical = initial;
    try {
        seams.coordinatorWait(1);
        const auto paused = transport(*fleet);
        require(paused.submitted == 63 && paused.generated == 256 && !paused.broken, "above-low stalled C/A");
        seams.released.store(1, std::memory_order_release);
        seams.coordinatorWait(2);
        await([&] { return seams.parked.load(std::memory_order_acquire); }, "producer did not enter in-flight tile");
        require(es_rt_submit(fleet.get(), &future) == ES_RT_OK, "C could not enqueue while W was stalled");
        seams.released.store(2, std::memory_order_release);
        seams.coordinatorWait(3);
        const auto broken = transport(*fleet);
        require(broken.submitted == 283 && broken.generated == 256 && broken.sealed == 320 && broken.broken,
                "fallback D became visible without the sticky break");
        es_rt_token refused{9, 2, 1, 0};
        require(prepare(*fleet, model, refused) == ES_RT_STATE && refused.epoch == 9 &&
                es_rt_submit(fleet.get(), &future) == ES_RT_STATE, "broken epoch accepted new work");
        seams.unblock.store(true, std::memory_order_release);
        seams.released.store(3, std::memory_order_release);
        seams.coordinatorWait(4);
    } catch (...) {
        seams.unblock.store(true, std::memory_order_release);
        seams.released.store(4, std::memory_order_release);
        audio.join();
        if (error) std::rethrow_exception(error);
        throw;
    }
    audio.join();
    require(es_rt_quiesce(fleet.get()) == ES_RT_OK, "join broken real transport");
    const auto stopped = transport(*fleet);
    require(stopped.submitted == 347 && stopped.generated == 256 && stopped.broken && stopped.returned_cells == 2,
            "late W published stale audio or retained overlap");
    receipt(*fleet, first, ES_RT_OK, 0, 0);
    receipt(*fleet, second, ES_RT_CANCELED, 63, ES_RT_NO_FRAME);
    receipt(*fleet, future, ES_RT_CANCELED, ES_RT_NO_FRAME, ES_RT_NO_FRAME);
    collectAll(*fleet, 2);
    std::cout << "broken epoch=" << epoch << " D=251 pcm=5 fallback=27 late-G=256 next-fallback=64 final-D=347\n";
}

void fresh(const fs::path &model) {
    constexpr auto previous = epochBase - 1;
    auto lost = create(previous);
    const auto token = prepare(*lost, model);
    const auto started = activate(*lost, token);
    require(es_rt_start(lost.get(), nullptr) == ES_RT_OK, "prime before simulated device loss");
    require(es_rt_quiesce(lost.get()) == ES_RT_OK, "device loss before first callback");
    require(transport(*lost).submitted == 0 && transport(*lost).generated == 256 && !lost->pcm.broken(),
            "device loss invented playback");
    receipt(*lost, started, ES_RT_OK, 0, 0);
    collectAll(*lost, 1);
    lost.reset();

    auto fleet = create(epochBase, 1024);
    require(es_rt_discard(fleet.get(), token) == ES_RT_STALE &&
            es_rt_submit(fleet.get(), &started) == ES_RT_STALE, "old epoch token or event reused");
    const auto first = activate(*fleet, prepare(*fleet, model));
    const auto second = swap(*fleet, prepare(*fleet, model), 64, 0);
    require(es_rt_start(fleet.get(), nullptr) == ES_RT_OK, "prime the new epoch");
    DeviceBlock output{*fleet};
    std::exception_ptr error;
    std::thread audio([&] {
        try {
            output.process(q, q);
            // Replacement has completed before this sample starts; guard the first
            // call and every call across the new generation with the same probe.
            for (unsigned frame = q; frame < 4096; frame += q) output.process(q, q);
        } catch (...) {
            error = std::current_exception();
        }
    });
    audio.join();
    if (error) std::rethrow_exception(error);
    require(output.next == 4096 && output.useful != 0 && !transport(*fleet).broken, "fresh history did not start at zero");
    require(es_rt_quiesce(fleet.get()) == ES_RT_OK, "quiesce new epoch");
    receipt(*fleet, first, ES_RT_OK, 0, 0);
    receipt(*fleet, second, ES_RT_OK, q, q);
    collectAll(*fleet, 2);
    std::cout << "fresh epoch=" << epochBase << " first=0 frames=4096 useful=" << output.useful
              << " older-epoch-replay=0 owner-cells=0\n";
}
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    try {
        const auto model = fs::absolute(argv[2]);
        const std::string_view mode = argv[1];
        if (mode == "epoch") {
            allSpans();
            stalePartial();
            corruptCredit();
            nearWrap();
            nativeBroken(model, epochBase + 80, false);
            fresh(model);
        } else if (mode == "stress") {
            for (unsigned n = 0; n < 16; ++n) nativeBroken(model, epochBase + 1 + n, n == 0);
            fresh(model);
        } else return 2;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
