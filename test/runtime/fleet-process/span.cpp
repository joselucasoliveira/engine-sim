#include "support.hpp"

#include <algorithm>
#include <cstdio>
#include <thread>

namespace engine_sim::fleet {
struct PcmTransportTestAccess {
    static std::uint64_t published(const PcmTransport &transport) {
        return transport.m_write.load(std::memory_order_relaxed);
    }
};
}

namespace {
using namespace fleet_process_test;
constexpr auto all = 0x1ffu;
constexpr auto quantum = engine_sim::fleet::tileFrames;

void span(es_rt_fleet *fleet, unsigned count, unsigned mask, std::uint64_t first, unsigned prefix) {
    Call call(count, mask);
    call.check(es_rt_process(fleet, &call.audio, &call.block), first, prefix);
    require(fleet->commands.submitted.load() == first + count, "D does not describe the full submitted span");
}

void fill(es_rt_fleet *fleet, unsigned count, std::uint64_t first = 0) {
    for (unsigned i = 0; i < count; ++i)
        require(fleet->pcm.tryPublish(tile(first + i * quantum)), "publish a free slot");
}

void preflight() {
    auto owner = create(1024, 2);
    auto *fleet = owner.get();
    Call call(64, 7);
    call.rejected(nullptr, ES_RT_INVALID, &call.audio, &call.block);
    call.rejected(fleet, ES_RT_INVALID, nullptr, &call.block);
    call.rejected(fleet, ES_RT_INVALID, &call.audio, nullptr);
    --call.audio.size;
    call.rejected(fleet, ES_RT_CAPACITY, &call.audio, &call.block);
    ++call.audio.size;
    call.audio.abi = ES_RT_V1 + 1;
    call.rejected(fleet, ES_RT_VERSION, &call.audio, &call.block);
    call.audio.abi = ES_RT_V1;
    call.block.size = sizeof(std::uint32_t);
    call.rejected(fleet, ES_RT_CAPACITY, &call.audio, &call.block);
    call.block.size = sizeof(call.block);
    call.block.abi = ES_RT_V1 + 1;
    call.rejected(fleet, ES_RT_VERSION, &call.audio, &call.block);
    call.block.abi = ES_RT_V1;
    call.audio.frames = 1025;
    call.audio.capacity = 1025;
    call.rejected(fleet, ES_RT_CAPACITY, &call.audio, &call.block);
    call.audio.frames = 65;
    call.audio.capacity = 64;
    call.rejected(fleet, ES_RT_CAPACITY, &call.audio, &call.block);
    call.audio.capacity = capacity;
    call.audio.stem[7] = call.output[8].data() + 1;
    call.rejected(fleet, ES_RT_INVALID, &call.audio, &call.block);
    call.audio.frames = 0;
    call.rejected(fleet, ES_RT_INVALID, &call.audio, &call.block);
    Call missing(1, 0);
    missing.rejected(fleet, ES_RT_INVALID, &missing.audio, &missing.block);
    Call overflow(3, 1);
    const auto near = std::numeric_limits<std::uint64_t>::max() - 2;
    fleet->commands.submitted.store(near);
    overflow.rejected(fleet, ES_RT_CAPACITY, &overflow.audio, &overflow.block);
    span(fleet, 0, 0, near, 0);
    require(!fleet->pcm.broken(), "rejected and empty spans break transport");
}

void partitionAndWrap() {
    for (const auto maximum : {64u, 128u, 256u, 512u, 1024u}) {
        for (const auto profile : {ES_RT_BALANCED, ES_RT_INTERACTIVE}) {
            if (profile == ES_RT_INTERACTIVE && maximum > 128) continue;
            auto owner = create(maximum, 8, profile);
            auto *fleet = owner.get();
            const auto slots = (profile == ES_RT_INTERACTIVE ? 3 : 4) * maximum / quantum + 2;
            span(fleet, 0, 0, 0, 0);
            fill(fleet, slots);
            const std::uint64_t full = slots * quantum;
            require(!fleet->pcm.tryPublish(tile(full)), "full ring refused an overwrite");
            span(fleet, 0, all, 0, 0);
            span(fleet, 1, 1, 0, 1);
            require(!fleet->pcm.tryPublish(tile(full)), "partial slot remained borrowed");
            span(fleet, 63, 0x100, 1, 63);
            fill(fleet, 1, full);
            span(fleet, 64, 0x1ac, 64, 64);
            fill(fleet, 1, full + 64);
            const unsigned crossing = std::min(65u, maximum);
            span(fleet, crossing, all, 128, crossing);
            span(fleet, maximum, all, 128 + crossing, maximum);
            Call tooLong(maximum + 1, all);
            tooLong.rejected(fleet, ES_RT_CAPACITY, &tooLong.audio, &tooLong.block);
            const auto next = 128u + crossing + maximum;
            for (std::uint64_t frame = next; frame < full + 128;) {
                const auto count = static_cast<unsigned>(std::min<std::uint64_t>(maximum, full + 128 - frame));
                span(fleet, count, all, frame, count);
                frame += count;
            }
            require(!fleet->pcm.broken(), "healthy partitions and physical wrap broke transport");
        }
    }
}

void breaks() {
    for (unsigned cause = 0; cause < 5; ++cause) {
        auto owner = create(256, 8);
        auto *fleet = owner.get();
        if (cause != 0) fill(fleet, 1);
        if (cause >= 2) {
            auto invalid = tile(64);
            if (cause == 2) invalid.epoch = epoch + 1;
            if (cause == 3) invalid.first = 128;
            if (cause == 4) invalid.first = 0;
            require(fleet->pcm.tryPublish(invalid), "install malformed tag");
            fill(fleet, 1, 128);
        }
        const unsigned prefix = cause ? 64 : 0;
        span(fleet, 128, all, 0, prefix);
        require(fleet->pcm.broken(), "missing, stale or skipped tile must break");
        require(!fleet->pcm.tryPublish(tile(128)), "sticky break refused new producer data");
        span(fleet, 0, 0, 128, 0);
        span(fleet, 65, all, 128, 0);
        std::printf("break case=%u first=0 pcm=%u fallback=%u subsequent_pcm=0 subsequent_fallback=65\n",
                    cause, prefix, 128 - prefix);
    }
}

void race() {
    auto owner = create(128, 8);
    auto *fleet = owner.get();
    constexpr auto count = 16384u;
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (unsigned i = 0; i < count && !stop.load(std::memory_order_relaxed); ++i) {
            const auto audio = tile(static_cast<std::uint64_t>(i) * quantum);
            while (!fleet->pcm.tryPublish(audio)) {
                if (stop.load(std::memory_order_relaxed)) return;
                std::this_thread::yield();
            }
        }
    });
    const auto wait = [&](unsigned published) {
        // Poll the same index without acquire so the callback supplies the actual
        // synchronization when it takes its later snapshot.
        while (engine_sim::fleet::PcmTransportTestAccess::published(fleet->pcm) < published)
            std::this_thread::yield();
    };
    try {
        for (unsigned i = 0; i < count; i += 2) {
            const auto first = static_cast<std::uint64_t>(i) * quantum;
            wait(i + 1);
            span(fleet, 13, all, first, 13);
            wait(i + 2);
            span(fleet, 2 * quantum - 13, all, first + 13, 2 * quantum - 13);
        }
    } catch (...) {
        stop.store(true, std::memory_order_relaxed);
        writer.join();
        throw;
    }
    writer.join();
    require(!fleet->pcm.broken(), "SPSC transport broke");
    std::printf("race published=%u submitted=%u slots=10\n", count, count * quantum);
}
}

int main() {
    try {
        preflight();
        partitionAndWrap();
        breaks();
        race();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
