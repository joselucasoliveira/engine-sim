#include "owner.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace engine_sim::fleet {
struct IngressTestAccess {
    static void nearWrap(Ingress &queue) {
        queue.head.store(UINT64_MAX - 1, std::memory_order_relaxed);
        queue.tail.store(UINT64_MAX - 1, std::memory_order_relaxed);
    }
};
}

namespace {
namespace fs = std::filesystem;
namespace core = engine_sim::fleet;
using Fleet = std::unique_ptr<es_rt_fleet, decltype(&es_rt_destroy)>;
constexpr auto epoch = std::uint64_t{403};
constexpr auto default_rate = 48000;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
Fleet owner() {
    const es_rt_config config{sizeof(config), ES_RT_V1, default_rate, ES_RT_BALANCED, 64, 2, ES_RT_EXPERIMENTAL, 0, epoch};
    es_rt_fleet *value = nullptr;
    require(es_rt_create(&config, &value, nullptr) == ES_RT_OK, "Could not create test fleet");
    return Fleet(value, es_rt_destroy);
}
es_rt_token prepare(es_rt_fleet &fleet, const fs::path &file) {
    const es_rt_voice_config voice{sizeof(voice), ES_RT_V1, ES_RT_FREE, 0, ES_RT_RAW, 0, 403, 0, {}};
    es_rt_token token{};
    const auto path = file.u8string(), root = file.parent_path().u8string();
    require(es_rt_prepare_file(&fleet, &voice, path.c_str(), root.c_str(), &token, nullptr) == ES_RT_OK,
            "Could not prepare ingress fixture");
    return token;
}
es_rt_event event(std::uint64_t sequence, std::uint64_t frame, unsigned kind) {
    es_rt_event result{};
    result.size = sizeof(result); result.abi = ES_RT_V1;
    result.epoch = epoch; result.sequence = sequence; result.frame = frame; result.kind = kind;
    return result;
}
es_rt_event master(std::uint64_t sequence, std::uint64_t frame) {
    auto result = event(sequence, frame, ES_RT_MASTER);
    result.gain = static_cast<double>(sequence % 16) / 16;
    return result;
}
void submit(es_rt_fleet &fleet, const es_rt_event &value, std::int32_t expected = ES_RT_OK) {
    const auto status = es_rt_submit(&fleet, &value);
    if (status != expected) std::cerr << "sequence=" << value.sequence << " frame=" << value.frame << " kind=" << value.kind
                                      << " expected=" << expected << " actual=" << status << '\n';
    require(status == expected, "Unexpected command submit status");
}
void poll(es_rt_fleet &fleet, std::uint64_t sequence, std::int32_t status = ES_RT_OK,
          std::uint32_t reason = ES_RT_OK, std::uint64_t applied = ES_RT_NO_FRAME,
          std::uint64_t ended = ES_RT_NO_FRAME) {
    struct Output { es_rt_result known; std::uint64_t tail; } output{};
    output.known.size = sizeof(output); output.known.abi = ES_RT_V1;
    output.known.sequence = UINT64_MAX;
    output.tail = UINT64_MAX - 1;
    const auto original = output;
    require(es_rt_poll_result(&fleet, sequence, &output.known) == status, "Unexpected poll status");
    if (status == ES_RT_OK) {
        require(output.known.sequence == sequence && output.known.reason == reason && output.known.epoch == epoch &&
            output.known.applied == applied && output.known.ended == ended && output.tail == original.tail &&
            output.known.size == original.known.size && output.known.abi == ES_RT_V1, "Terminal receipt was lost or changed");
    } else {
        require(std::memcmp(&output, &original, sizeof(output)) == 0, "An unavailable receipt changed caller storage");
    }
}
core::Command take(es_rt_fleet &fleet, std::uint64_t at, std::uint32_t reason = ES_RT_OK,
                   std::uint64_t applied = ES_RT_NO_FRAME, std::uint64_t ended = ES_RT_NO_FRAME) {
    auto &queue = fleet.commands.ingress;
    const auto *borrowed = queue.peek(queue.snapshot());
    if (!borrowed || borrowed->event.sequence != at)
        throw std::runtime_error("Missing or overwritten FIFO head");
    const auto command = *borrowed;
    queue.consume();
    core::finishCommand(fleet, command, reason, applied, ended);
    return command;
}

void saturation(const fs::path &file) {
    auto fleet = owner();
    const auto token = prepare(*fleet, file);
    auto activate = event(1, 0, ES_RT_ACTIVATE);
    activate.next_voice = {1, 0, 0}; activate.candidate = token;
    submit(*fleet, activate);
    poll(*fleet, 1, ES_RT_PENDING);
    require(es_rt_discard(fleet.get(), token) == ES_RT_STALE, "Queued token could still be discarded");
    take(*fleet, 1, ES_RT_OK, 0, 0);
    poll(*fleet, 1, ES_RT_OK, ES_RT_OK, 0, 0);
    const auto initial = fleet->commands.ingress.writeIndex();
    for (unsigned i = 0; i < 248; ++i) {
        auto value = master(i + 2, (i / 12 + 1) * core::quantum);
        submit(*fleet, value);
        value.gain = std::numeric_limits<double>::quiet_NaN();
        value.sequence = 99;
    }
    auto ordinary = master(250, fleet->commands.lastFrame);
    submit(*fleet, ordinary, ES_RT_FULL);
    for (unsigned i = 0; i < 8; ++i) {
        auto mute = event(250 + i, fleet->commands.lastFrame, ES_RT_MUTE);
        mute.voice = {1, 0, 0};
        submit(*fleet, mute);
    }
    auto full = event(258, fleet->commands.lastFrame + core::quantum, ES_RT_RESUME);
    full.voice = {1, 0, 0};
    submit(*fleet, full, ES_RT_FULL);
    require(fleet->commands.ingress.writeIndex() == initial + 256 && fleet->commands.outstanding == 256,
            "A rejected command consumed queue or ticket space");
    for (unsigned sequence = 2; sequence < 258; ++sequence) {
        const auto work = take(*fleet, sequence, ES_RT_OK, sequence, sequence + 1);
        if (sequence < 250) require(work.event.gain == master(sequence, 0).gain, "Caller mutation changed owned payload");
    }
    // The queue is empty, but every done ticket still belongs to the C reader.
    require(fleet->commands.ingress.acquireHead() == fleet->commands.ingress.writeIndex(), "Failed to drain physical ingress");
    submit(*fleet, full, ES_RT_FULL);
    for (unsigned sequence = 2; sequence < 258; ++sequence) {
        poll(*fleet, sequence, ES_RT_OK, ES_RT_OK, sequence, sequence + 1);
        poll(*fleet, sequence, ES_RT_STALE);
    }
    submit(*fleet, full);
    take(*fleet, 258, ES_RT_INTERRUPTED, 2000, 2003);
    poll(*fleet, 258, ES_RT_OK, ES_RT_INTERRUPTED, 2000, 2003);
    std::cout << "248 ordinary and 8 reserved entries retained through a reader stall.\n";
}

void ordering() {
    auto fleet = owner();
    for (unsigned i = 0; i < 12; ++i) submit(*fleet, master(i + 1, 64));
    submit(*fleet, master(13, 65), ES_RT_FULL);
    submit(*fleet, master(12, 128), ES_RT_ORDER);
    submit(*fleet, master(13, 63), ES_RT_ORDER);
    auto wrong = master(13, 128); wrong.epoch = epoch - 1;
    submit(*fleet, wrong, ES_RT_STALE);
    wrong.epoch = epoch;
    wrong.voice.generation = 1;
    submit(*fleet, wrong, ES_RT_INVALID);
    fleet->commands.sealed = 128;
    submit(*fleet, master(13, 127), ES_RT_LATE);
    submit(*fleet, master(13, 2 * default_rate + 1), ES_RT_INVALID);
    submit(*fleet, master(13, 128));
    poll(*fleet, 13, ES_RT_PENDING);
    poll(*fleet, 195, ES_RT_STALE);
    auto exhausted = owner();
    exhausted->commands.lastSequence = UINT64_MAX - 1;
    exhausted->commands.submitted = UINT64_MAX - 128;
    core::IngressTestAccess::nearWrap(exhausted->commands.ingress);
    submit(*exhausted, master(UINT64_MAX, UINT64_MAX - 63), ES_RT_CAPACITY);
    submit(*exhausted, master(UINT64_MAX, UINT64_MAX - 64));
    take(*exhausted, UINT64_MAX);
    poll(*exhausted, UINT64_MAX);
    submit(*exhausted, master(0, UINT64_MAX - 64), ES_RT_CAPACITY);
    require(exhausted->commands.ingress.writeIndex() == UINT64_MAX && exhausted->commands.ingress.acquireHead() == UINT64_MAX,
            "Absolute ring indices silently wrapped");
    std::cout << "Rejected stale epoch, duplicate, retrograde, sealed, future and exhausted cursor requests.\n";
}

void generations(const fs::path &file) {
    auto fleet = owner();
    auto bad = prepare(*fleet, file);
    auto activate = event(1, 0, ES_RT_ACTIVATE);
    activate.next_voice = {1, 0, 0}; activate.candidate = bad;
    submit(*fleet, activate);
    take(*fleet, 1, ES_RT_CANCELED);
    require(fleet->cells[bad.cell].state.load(std::memory_order_acquire) == core::CellState::returned &&
        fleet->cells[bad.cell].voice != nullptr && fleet->prepared == 1,
        "W released a candidate or its coordinator credit early");
    poll(*fleet, 1, ES_RT_OK, ES_RT_CANCELED);
    require(fleet->prepared == 0 && fleet->commands.lanes[0].highWater == 1, "Cancellation rolled back lane high-water");
    auto ready = prepare(*fleet, file);
    activate.sequence = 2; activate.candidate = ready;
    submit(*fleet, activate, ES_RT_STALE);
    activate.next_voice.generation = 2;
    submit(*fleet, activate);
    take(*fleet, 2, ES_RT_OK, 0, 0);
    poll(*fleet, 2, ES_RT_OK, ES_RT_OK, 0, 0);

    auto next = prepare(*fleet, file), retained = prepare(*fleet, file);
    auto replace = event(3, 64, ES_RT_REPLACE);
    replace.voice = {2, 0, 0}; replace.next_voice = {3, 0, 0}; replace.candidate = next;
    submit(*fleet, replace);
    replace.sequence = 4; replace.next_voice.generation = 4; replace.candidate = retained;
    submit(*fleet, replace, ES_RT_BUSY);
    require(fleet->cells[retained.cell].state.load() == core::CellState::prepared, "Busy overlap transferred candidate");
    take(*fleet, 3, ES_RT_STATE);
    poll(*fleet, 3, ES_RT_OK, ES_RT_STATE);
    require(fleet->commands.lanes[0].current.generation == 2 && !fleet->commands.overlap,
            "Rejected replacement lost the old identity or kept its credit");
    submit(*fleet, replace);
    take(*fleet, 4, ES_RT_OK, 64, 70);
    poll(*fleet, 4, ES_RT_OK, ES_RT_OK, 64, 70);
    require(fleet->cells[ready.cell].state.load() == core::CellState::returned &&
        fleet->commands.lanes[0].current.generation == 4, "Replacement transit lost retired ownership or active generation");
    auto vector = event(5, 128, ES_RT_VECTOR);
    vector.voice = {2, 0, 0};
    submit(*fleet, vector, ES_RT_STALE);
    vector.voice = {4, 0, 0}; vector.physical.driven_rpm = 100;
    submit(*fleet, vector, ES_RT_UNSUPPORTED);
    vector.physical.driven_rpm = 0.;
    submit(*fleet, vector);
    take(*fleet, 5, ES_RT_OK, 128, 128);
    poll(*fleet, 5, ES_RT_OK, ES_RT_OK, 128, 128);
    auto last = prepare(*fleet, file);
    fleet->commands.lanes[1].highWater = UINT64_MAX;
    activate.sequence = 6; activate.frame = 128; activate.next_voice = {UINT64_MAX, 1, 0}; activate.candidate = last;
    submit(*fleet, activate, ES_RT_CAPACITY);
    require(es_rt_discard(fleet.get(), last) == ES_RT_OK, "Exhaustion released or transferred a retained token");
    replace.sequence = 6; replace.frame = 128; replace.voice = {4, 0, 0}; replace.next_voice = {5, 0, 0};
    replace.candidate = prepare(*fleet, file);
    submit(*fleet, replace);
    auto stop = event(7, 192, ES_RT_STOP);
    stop.voice = replace.next_voice;
    submit(*fleet, stop);
    take(*fleet, 6, ES_RT_OK, 128, 192);
    take(*fleet, 7, ES_RT_OK, 192, 192);
    poll(*fleet, 7, ES_RT_OK, ES_RT_OK, 192, 192);
    poll(*fleet, 6, ES_RT_OK, ES_RT_OK, 128, 192);
    require(fleet->commands.lanes[0].current.generation == 0 && !fleet->commands.overlap && fleet->prepared == 0,
            "Reaping a newer terminal ticket first resurrected a retired voice");
    std::cout << "Cancellation and replacement preserved monotonic generations, returned cells and overlap credits.\n";
}

void race() {
    constexpr std::uint64_t total = 8192;
    auto fleet = owner();
    std::atomic<unsigned> phase{0};
    std::atomic<bool> abort{false};
    std::exception_ptr worker_failure;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    const auto pause = [&] {
        require(!abort.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline,
                "Worker or coordinator did not reach the handoff barrier");
        std::this_thread::yield();
    };
    const auto await = [&](unsigned wanted) { while (phase.load(std::memory_order_acquire) < wanted) pause(); };
    const auto expected = [](std::uint64_t sequence) { return master(sequence, ((sequence - 1) / 12) * core::quantum); };
    submit(*fleet, expected(1));
    auto worker = std::thread([&] {
        try {
            auto &queue = fleet->commands.ingress;
            const auto first_tail = queue.snapshot();
            const auto *borrow = queue.peek(first_tail);
            if (!borrow || borrow->event.sequence != 1)
                throw std::runtime_error("Did not acquire first published payload");
            phase.store(1, std::memory_order_release);
            await(2);
            require(borrow->event.sequence == 1 && borrow->event.gain == expected(1).gain, "C overwrote an acquired slot");
            require(first_tail == 1, "Worker's ingress snapshot grew across stall");
            const auto first = *borrow;
            queue.consume();
            require(queue.peek(first_tail) == nullptr, "Worker read beyond the acquired tail");
            core::finishCommand(*fleet, first, ES_RT_OK, 1, 1);
            phase.store(3, std::memory_order_release);
            await(4);
            std::uint64_t sequence = 2;
            while (sequence <= total) {
                const auto limit = queue.snapshot();
                while (const auto *next = queue.peek(limit)) {
                    const auto owned = *next;
                    const auto wanted = expected(sequence);
                    require(owned.event.sequence == sequence && owned.event.frame == wanted.frame &&
                        owned.event.gain == wanted.gain, "Wrong payload after ring storage wrap");
                    queue.consume();
                    core::finishCommand(*fleet, owned, sequence % 5 == 0 ? ES_RT_CANCELED : ES_RT_OK,
                                        sequence, sequence);
                    ++sequence;
                }
                if (sequence <= total) pause();
            }
        } catch (...) {
            worker_failure = std::current_exception();
            abort.store(true, std::memory_order_release);
        }
    });
    try {
        await(1);
        for (std::uint64_t i = 2; i <= 248; ++i) submit(*fleet, expected(i));
        submit(*fleet, expected(249), ES_RT_FULL);
        poll(*fleet, 1, ES_RT_PENDING);
        phase.store(2, std::memory_order_release);
        await(3);
        poll(*fleet, 1, ES_RT_OK, ES_RT_OK, 1, 1);
        submit(*fleet, expected(249));
        phase.store(4, std::memory_order_release);
        std::uint64_t next = 250, read = 2;
        while (read <= total) {
            bool progress = false;
            if (next <= total) {
                const auto value = expected(next);
                const auto status = es_rt_submit(fleet.get(), &value);
                require(status == ES_RT_OK || status == ES_RT_FULL, "Unexpected status under actual concurrent ingress");
                if (status == ES_RT_OK) { ++next; progress = true; }
            }
            if (read < next) {
                es_rt_result result{};
                result.size = sizeof(result); result.abi = ES_RT_V1;
                const auto status = es_rt_poll_result(fleet.get(), read, &result);
                require(status == ES_RT_OK || status == ES_RT_PENDING, "Outstanding mandatory ticket was lost");
                if (status == ES_RT_OK) {
                    require(result.applied == read && result.ended == read && result.requested == expected(read).frame &&
                        result.epoch == epoch && result.sequence == read &&
                        result.reason == (read % 5 == 0 ? ES_RT_CANCELED : ES_RT_OK), "Concurrent receipt was torn");
                    ++read; progress = true;
                }
            }
            if (!progress) pause();
        }
    } catch (...) {
        abort.store(true, std::memory_order_release);
        worker.join();
        if (worker_failure) std::rethrow_exception(worker_failure);
        throw;
    }
    worker.join();
    if (worker_failure) std::rethrow_exception(worker_failure);
    require(fleet->commands.outstanding == 0 && fleet->commands.ingress.writeIndex() == total &&
        fleet->commands.ingress.acquireHead() == total, "Concurrent ring or ticket totals differ");
    std::cout << "8192 copied commands and mandatory completions, 32 physical ring wraps, 1638 canceled.\n";
}
}

int main(int argc, char **argv) try {
    require(argc == 3, "Expected ingress selector and model");
    if (std::string_view(argv[1]) == "bounds") {
        const auto file = fs::absolute(argv[2]);
        ordering(); saturation(file); generations(file);
    } else race();
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
