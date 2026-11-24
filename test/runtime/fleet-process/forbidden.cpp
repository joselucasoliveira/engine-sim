#include "boundary.hpp"
#include "observe.hpp"
#include "support.hpp"

#include <cstdio>
#include <dlfcn.h>
#include <string_view>
#include <thread>

namespace {
using namespace fleet_process_test;
using realtime_probe::Operation;

void run(ProcessBoundary boundary, std::string_view scenario) {
    auto owner = create(128, 8);
    const auto zero = scenario == "zero";
    const auto invalid = scenario == "invalid";
    const auto stale = scenario == "stale";
    const auto empty = scenario == "starved";
    const auto shortPrefix = scenario == "short";
    const auto inject = scenario == "inject";
    if (!empty && !zero) require(owner->pcm.tryPublish(tile(0)), "first tile setup");
    if (!empty && !zero && !shortPrefix) {
        auto second = tile(64);
        if (stale) ++second.epoch;
        require(owner->pcm.tryPublish(second), "second tile setup");
    }
    Call call(zero ? 0 : 65, zero ? 0 : 0x1ff);
    if (invalid) call.audio.abi = ES_RT_V1 + 1;
    const auto before = call.block;
    const bool broken = stale || shortPrefix || empty;
    Call fallback(65, 0x1ff);
    realtime_probe::Record hits{}, following{};
    int result = -1, fallbackResult = -1;
    std::thread callback([&] {
        {
            const realtime_probe::Scope scope(hits);
            result = boundary(owner.get(), &call.audio, &call.block, inject);
        }
        if (broken) {
            const realtime_probe::Scope scope(following);
            fallbackResult = boundary(owner.get(), &fallback.audio, &fallback.block, false);
        }
    });
    callback.join();
    if (invalid) {
        require(result == ES_RT_VERSION && std::memcmp(&before, &call.block, sizeof(before)) == 0,
                "bad callback prefix");
        for (const auto &plane : call.output) {
            for (float value : plane) require(value == sentinel, "bad prefix changed output");
        }
    } else {
        call.check(result, 0, zero || empty ? 0 : stale || shortPrefix ? 64 : 65);
    }
    realtime_probe::dump(hits);
    for (std::size_t i = 0; i < hits.size(); ++i) {
        const auto operation = static_cast<Operation>(i);
        const bool injected = inject && (operation == Operation::malloc || operation == Operation::free ||
            operation == Operation::pthread_mutex_lock || operation == Operation::formatted_output);
        require(hits[i].count == (injected ? 1u : 0u), realtime_probe::names[i]);
    }
    if (broken) {
        fallback.check(fallbackResult, 65, 0);
        for (const auto &hit : following) require(!hit.count, "forbidden operation during sticky fallback");
    }
    if (invalid) {
        std::printf("observed invalid preserved result and output\n");
    } else {
        std::printf("observed %s pcm=%u fallback=%u\n", scenario.data(), call.block.pcm_frames, call.block.fallback_frames);
    }
}
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    try {
        const std::string_view boundary = argv[1], scenario = argv[2];
        require(boundary == "direct" || boundary == "shared", "unknown probe boundary");
        require(scenario == "nominal" || scenario == "zero" || scenario == "invalid" ||
                scenario == "stale" || scenario == "starved" || scenario == "short" || scenario == "inject",
                "unknown probe scenario");
        if (boundary == "direct") {
            run(fleetProcessDirect, scenario);
        } else {
            // Resolve the shared entry before the callback thread exists.
            const auto close = [](void *handle) { dlclose(handle); };
            const std::unique_ptr<void, decltype(close)> library(dlopen(argv[3], RTLD_NOW | RTLD_LOCAL), close);
            if (!library) throw std::runtime_error(dlerror());
            auto entry = reinterpret_cast<ProcessBoundary>(dlsym(library.get(), "fleetProcessShared"));
            require(entry != nullptr, "missing shared callback boundary");
            run(entry, scenario);
        }
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
