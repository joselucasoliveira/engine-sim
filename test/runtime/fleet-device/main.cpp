#include "adapter.hpp"
#include "../fleet-process/support.hpp"

#ifdef ES_FLEET_ADAPTER_OBSERVER
#include "escape.hpp"
#include "observe.hpp"
#endif

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <thread>
#include <iostream>
#include <vector>

namespace {
using namespace fleet_example;
using fleet_process_test::create;
using fleet_process_test::sample;
using fleet_process_test::tile;
using fleet_process_test::require;
constexpr auto markedXrun = 0x20u;
constexpr StreamTimes timing{1., 1.01, 1.02};

CallbackReply invoke(Adapter &adapter, float *out, unsigned n, std::uint64_t flags = 0, bool inject = false) {
#ifdef ES_FLEET_ADAPTER_OBSERVER
    realtime_probe::Record record{};
    CallbackReply reply{};
    {
        const realtime_probe::Scope scope(record);
        if (inject) {
            void *memory = std::malloc(64);
            escape(memory);
            std::free(memory);
        }
        reply = adapter.process(out, n, &timing, flags);
    }
    for (std::size_t i = 0; i < record.size(); ++i) {
        using Op = realtime_probe::Operation;
        const auto operation = static_cast<Op>(i);
        const bool control = inject && (operation == Op::malloc || operation == Op::free);
        require(record[i].count == (control ? 1u : 0u), realtime_probe::names[i]);
    }
    if (inject) realtime_probe::dump(record);
    return reply;
#else
    (void)inject;
    return adapter.process(out, n, &timing, flags);
#endif
}

CallbackRecord saved(Adapter &adapter) {
    CallbackRecord record{};
    require(adapter.poll(record), "missing callback witness");
    return record;
}

void record(const CallbackRecord &entry, CallbackCause cause, std::uint64_t first,
            unsigned frames, unsigned pcm, unsigned fallback, unsigned flags) {
    require(entry.timestamped && entry.times.input == timing.input && entry.times.current == timing.current &&
            entry.times.dac == timing.dac && entry.cause == cause && entry.first == first &&
            entry.frames == frames && entry.pcm == pcm && entry.fallback == fallback && entry.flags == flags,
            "host status or timer context lost");
}

struct Output {
    std::vector<float> samples;
    explicit Output(unsigned count) : samples(count + 2, 23.f) {}
    float *data() { return samples.data() + 1; }
    void guards() const {
        require(samples.front() == 23.f && samples.back() == 23.f, "device span bounds");
    }
    void silence() const {
        guards();
        require(std::all_of(samples.begin() + 1, samples.end() - 1, [](float value) {
            const float zero = 0;
            return std::memcmp(&value, &zero, sizeof(zero)) == 0;
        }), "aborting device span was not entirely initialized");
    }
};

void mono() {
    auto owner = create(64, 2);
    require(owner->pcm.tryPublish(tile(0)), "mono test PCM");
    Adapter adapter(owner.get(), 64, 1, markedXrun, 64);
    Output out(64);
    std::exception_ptr failure;
    std::thread callback([&] {
        try {
            require(invoke(adapter, nullptr, 0) == CallbackReply::next, "empty device buffer");
            require(invoke(adapter, out.data(), 64, 0, true) == CallbackReply::complete, "complete mono block");
        } catch (...) {
            failure = std::current_exception();
        }
    });
    callback.join();
    if (failure) std::rethrow_exception(failure);
    record(saved(adapter), CallbackCause::normal, 0, 0, 0, 0, 0);
    const auto report = saved(adapter);
    record(report, CallbackCause::normal, 0, 64, 64, 0, 0);
    require(report.nonzero[0] == 63 && report.nonzero[1] == 63, "per-voice presence witness");
    for (unsigned i = 0; i < 64; ++i) {
        const auto expected = sample(0, i);
        require(std::memcmp(&out.samples[i+1], &expected, sizeof(float)) == 0, "mono passthrough");
    }
    out.guards();
}

void stereoPrefix() {
    auto owner = create(128, 2);
    require(owner->pcm.tryPublish(tile(0)), "stereo test PCM");
    Adapter adapter(owner.get(), 128, 2, markedXrun, 128);
    Output out(130);
    require(invoke(adapter, out.data(), 65) == CallbackReply::abort, "partial Fleet break in device callback");
    const auto report = saved(adapter);
    record(report, CallbackCause::fleet, 0, 65, 64, 1, 0);
    require(report.status == ES_RT_BROKEN && report.nonzero[0] == 63 && report.nonzero[1] == 63,
            "transport break prefix");
    for (unsigned i = 0; i < 65; ++i) {
        const auto expected = i < 64 ? sample(0, i) : 0.f;
        require(std::memcmp(&out.samples[2*i+1], &expected, sizeof(float)) == 0 &&
                std::memcmp(&out.samples[2*i+2], &expected, sizeof(float)) == 0, "stereo duplication");
    }
    out.guards();
    Output stopped(16);
    require(invoke(adapter, stopped.data(), 8) == CallbackReply::abort, "adapter break latch");
    record(saved(adapter), CallbackCause::backend, ES_RT_NO_FRAME, 8, 0, 0, 0);
    stopped.silence();
    require(owner->commands.submitted.load() == 65, "adapter synthesized presented progress after Fleet abort");
}

void guards() {
    struct Case { unsigned frames, channels, flags; CallbackCause cause; };
    for (const auto &test : std::array<Case, 4>{{
        {65, 1, 0, CallbackCause::period}, {1024, 2, 0, CallbackCause::period},
        {1025, 2, 0, CallbackCause::protocol}, {64, 2, markedXrun, CallbackCause::backend}
    }}) {
        auto owner = create(64, 2);
        Adapter adapter(owner.get(), 64, test.channels, markedXrun, 9000);
        Output output(test.frames * test.channels);
        require(invoke(adapter, output.data(), test.frames, test.flags) == CallbackReply::abort, "device abort branch");
        record(saved(adapter), test.cause, ES_RT_NO_FRAME, test.frames, 0, 0, test.flags);
        output.silence();
        require(owner->commands.submitted.load() == 0, "skipped callback advanced Fleet D");
    }
    auto owner = create(64, 2);
    Adapter adapter(owner.get(), 64, 1, markedXrun, 9000);
    require(invoke(adapter, nullptr, 1) == CallbackReply::abort, "backend omitted a positive output buffer");
    record(saved(adapter), CallbackCause::protocol, ES_RT_NO_FRAME, 1, 0, 0, 0);
    auto fresh = create(64, 2);
    Adapter replacement(fresh.get(), 64, 1, markedXrun, 9000);
    replacement.requestStop();
    Output stop(1);
    require(invoke(replacement, stop.data(), 1) == CallbackReply::abort, "control requested removal");
    record(saved(replacement), CallbackCause::backend, ES_RT_NO_FRAME, 1, 0, 0, 0);
    stop.silence();
    auto closed = create(64, 2);
    closed->producer.phase.store(engine_sim::fleet::FleetPhase::initial);
    Adapter preflight(closed.get(), 64, 2, markedXrun, 9000);
    Output rejected(2);
    require(invoke(preflight, rejected.data(), 1) == CallbackReply::abort, "Fleet state rejection initialized device");
    record(saved(preflight), CallbackCause::fleet, ES_RT_NO_FRAME, 1, 0, 0, 0);
    rejected.silence();
}

void timestamps() {
    auto owner = create(64, 2);
    Adapter adapter(owner.get(), 64, 1, markedXrun, 9000);
    for (unsigned i = 0; i < 263; ++i)
        require(invoke(adapter, nullptr, 0, i) == (i < markedXrun ? CallbackReply::next : CallbackReply::abort),
                "timestamp saturation callback");
    require(adapter.dropped() == 7, "drop-new callback count");
    for (unsigned i = 0; i < 256; ++i) require(saved(adapter).flags == i, "old callback time record overwritten");
    CallbackRecord exhausted{};
    require(!adapter.poll(exhausted), "timestamp queue capacity");

    Presentation map(48000);
    require(!map.calibrate(3., 1., 2.) && !std::isfinite(map.hostDacEstimate()) &&
            map.calibrate(2., 1., 2.002) && std::abs(map.uncertainty - .001) < 1e-12, "bracket calibration");
    CallbackRecord frame{};
    frame.timestamped = true;
    frame.first = 0;
    frame.frames = 48;
    frame.times = {0, 1.0, 1.02};
    require(map.observe(frame) && std::abs(map.hostDacEstimate() - 2.021) < 1e-12, "DAC estimate origin");
    frame.first = 48;
    frame.times.current += .001;
    frame.times.dac += .001;
    require(map.observe(frame) && std::abs(map.frameResidual) < 1e-8, "timestamp frame increment");
    frame.first = 96;
    frame.times.dac -= .002;
    require(!map.observe(frame) && !std::isfinite(map.hostDacEstimate()), "nonmonotone device clock accepted");
    frame.times.dac = 1.022;
    frame.cause = CallbackCause::backend;
    require(!map.observe(frame), "device discontinuity preserved a mapping");
}
}

int main() {
    try {
        mono(); stereoPrefix(); guards(); timestamps();
        std::cout << "mono stereo prefix=64 filler=1 period=65/1024 protocol=1025 drop-new=7"
                     " DAC bracket=0.001s native-endpoint=not-used\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
