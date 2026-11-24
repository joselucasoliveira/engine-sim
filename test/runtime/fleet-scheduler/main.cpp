#include "scheduler.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace fleet = engine_sim::fleet;
using Owner = std::unique_ptr<es_rt_fleet, decltype(&es_rt_destroy)>;
using Point = fleet::TimelinePoint;
constexpr std::uint64_t epoch = 731;

void require(bool valid, const char *message) {
    if (!valid) throw std::runtime_error(message);
}
Owner create(unsigned rate) {
    const es_rt_config config{sizeof(config), ES_RT_V1, rate, ES_RT_BALANCED, 64, 1, 0, 0, epoch};
    es_rt_fleet *value = nullptr;
    require(es_rt_create(&config, &value, nullptr) == ES_RT_OK, "Could not create scheduler owner");
    return Owner(value, es_rt_destroy);
}
es_rt_event command(std::uint64_t sequence, std::uint64_t frame, unsigned kind) {
    es_rt_event value{};
    value.size = sizeof(value); value.abi = ES_RT_V1; value.kind = kind;
    value.epoch = epoch; value.sequence = sequence; value.frame = frame;
    return value;
}
void submit(es_rt_fleet &owner, const es_rt_event &value, std::int32_t expect = ES_RT_OK) {
    const auto status = es_rt_submit(&owner, &value);
    if (status != expect) std::cerr << "event=" << value.sequence << " frame=" << value.frame << " status=" << status << '\n';
    require(status == expect, "Scheduler ingress returned wrong status");
}
void result(es_rt_fleet &owner, const es_rt_event &value, std::int32_t expect = ES_RT_OK,
            std::uint32_t reason = ES_RT_OK) {
    es_rt_result out{};
    out.size = sizeof(out); out.abi = ES_RT_V1;
    const auto status = es_rt_poll_result(&owner, value.sequence, &out);
    require(status == expect, "Scheduler receipt returned wrong status");
    if (expect != ES_RT_OK) return;
    const auto frame = reason == ES_RT_OK ? value.frame : ES_RT_NO_FRAME;
    require(out.epoch == epoch && out.sequence == value.sequence && out.requested == value.frame &&
        out.applied == frame && out.ended == frame && out.reason == reason, "Scheduler returned incorrect timing witness");
}
es_rt_token prepare(es_rt_fleet &owner, const fs::path &model, const es_rt_voice_config &settings) {
    es_rt_token value{};
    const auto file = model.u8string(), root = model.parent_path().u8string();
    require(es_rt_prepare_file(&owner, &settings, file.c_str(), root.c_str(), &value, nullptr) == ES_RT_OK,
            "Could not prepare independent scheduler model");
    return value;
}
es_rt_voice_config voice(unsigned frequency, bool driven) {
    const es_rt_vector initial{.25, 0, 0, 1, driven ? 0u : 1u};
    return {sizeof(es_rt_voice_config), ES_RT_V1, driven ? ES_RT_DRIVEN : ES_RT_FREE, frequency, ES_RT_RAW, 0,
            403, 0, initial};
}
struct Trace {
    std::array<Point, 128> points{};
    std::size_t size = 0;
    bool overflow = false;
    static void append(void *context, const Point &point) noexcept {
        auto &self = *static_cast<Trace *>(context);
        if (self.size == self.points.size()) { self.overflow = true; return; }
        self.points[self.size++] = point;
    }
};

class Driver {
public:
    explicit Driver(es_rt_fleet &owner, Trace *trace = nullptr)
        : scheduler(owner, trace ? Trace::append : nullptr, trace) {}

    void copy(float *destination, std::size_t count) {
        while (count != 0) {
            if (cursor == fleet::quantum) {
                require(scheduler.renderTile(tile) == ES_RT_OK, "Private worker tile was not produced");
                cursor = 0;
            }
            const auto size = std::min<std::size_t>(count, fleet::quantum - cursor);
            std::copy_n(tile.stem[0].data() + cursor, size, destination);
            cursor += size;
            destination += size;
            count -= size;
        }
    }
    fleet::Scheduler scheduler;
private:
    fleet::ScheduledTile tile;
    std::size_t cursor = fleet::quantum;
};

std::vector<es_rt_event> schedule(es_rt_fleet &owner, const fs::path &model, const es_rt_voice_config &settings,
                                 std::uint64_t start, const std::vector<std::uint64_t> &targets) {
    std::vector<es_rt_event> events;
    auto activate = command(1, start, ES_RT_ACTIVATE);
    activate.candidate = prepare(owner, model, settings);
    activate.next_voice = {1, 0, 0};
    events.push_back(activate);
    for (auto frame : targets) {
        auto value = command(events.size() + 1, frame, ES_RT_VECTOR);
        value.voice = activate.next_voice;
        value.physical = settings.initial;
        value.physical.demand = frame % 2 ? .35 : .53;
        value.physical.load_nm = frame % 3 ? .25 : 1;
        if (settings.mode == ES_RT_DRIVEN && frame >= 441) value.physical.driven_rpm = 120;
        if (frame > 500) value.physical.starter = 0;
        events.push_back(value);
    }
    for (const auto &value : events) submit(owner, value);
    return events;
}

void rationalWitness(const fs::path &model, unsigned rate, unsigned frequency) {
    auto owner = create(rate);
    constexpr std::uint64_t activation = 2, count = 192;
    const auto events = schedule(*owner, model, voice(frequency, false), activation,
                                 {2, 3, 3, 5, 6, 62, 63, 64, 64, 65, 66, 128});
    Trace trace;
    Driver driver(*owner, &trace);
    driver.copy(nullptr, 0);
    require(driver.scheduler.generated() == 0 && owner->commands.sealed == 0 && trace.size == 0,
            "A zero-sized request advanced worker time");
    std::array<float, count> data{};
    driver.copy(data.data(), data.size());
    for (const auto &value : events) result(*owner, value);

    std::vector<Point> expected;
    std::size_t edge = 0;
    std::uint64_t requested = 0, latched = 0, tick = 0;
    for (std::uint64_t frame = activation; frame < count; ++frame) {
        const auto local = frame - activation;
        while (edge < events.size() && events[edge].frame == frame) {
            requested = events[edge++].sequence;
            expected.push_back({Point::Kind::apply, requested, frame, local, ES_RT_NO_FRAME, {1, 0, 0}});
        }
        // Tick k starts at k/F. Integer division maps a right-edge tie to
        // the following interval, independently of SampleClock's state.
        while ((tick * rate) / frequency == local) {
            if (requested != latched) {
                latched = requested;
                expected.push_back({Point::Kind::latch, latched, frame, local, tick, {1, 0, 0}});
            }
            ++tick;
        }
    }
    require(!trace.overflow && trace.size == expected.size(), "Missing apply or coalesced-latch witness");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto &a = trace.points[i]; const auto &b = expected[i];
        require(a.kind == b.kind && a.sequence == b.sequence && a.frame == b.frame && a.localFrame == b.localFrame &&
            a.tick == b.tick && a.voice.generation == 1 && a.voice.lane == 0, "Timeline differs from rational tick oracle");
        std::cout << rate << '/' << frequency << ' ' << (a.kind == Point::Kind::latch ? "latch" : "apply")
                  << " sequence=" << a.sequence << " frame=" << a.frame << " local=" << a.localFrame
                  << " tick=" << a.tick << '\n';
    }
    const auto &source = *owner->cells[events.front().candidate.cell].voice;
    require(source.runtime.telemetry().ticks == (count - activation) * frequency / rate,
            "Worker local tick count differs from the completed-frame formula");
}

void sealRace() {
    auto owner = create(48000);
    auto first = command(1, 0, ES_RT_MASTER); first.gain = .5;
    submit(*owner, first);
    struct Injection { es_rt_fleet *owner; es_rt_event value; std::int32_t status = ES_RT_INTERNAL; } after{
        owner.get(), command(2, 63, ES_RT_MASTER)};
    fleet::Scheduler scheduler(*owner);
    fleet::ScheduledTile tile;
    require(scheduler.renderTile(tile, [](void *arg) noexcept {
        auto &request = *static_cast<Injection *>(arg);
        request.status = es_rt_submit(request.owner, &request.value);
    }, &after) == ES_RT_OK && after.status == ES_RT_OK, "Exact acquire/seal race did not admit the delayed command");
    result(*owner, first);
    result(*owner, after.value, ES_RT_PENDING);
    auto now = command(3, 63, ES_RT_MASTER);
    submit(*owner, now, ES_RT_LATE);
    now.frame = 64;
    submit(*owner, now);
    auto future = command(4, 128, ES_RT_MASTER);
    submit(*owner, future);
    require(scheduler.renderTile(tile) == ES_RT_OK, "Second tile failed");
    result(*owner, after.value, ES_RT_OK, ES_RT_LATE);
    result(*owner, now);
    result(*owner, future, ES_RT_PENDING);
    require(scheduler.renderTile(tile) == ES_RT_OK, "Third tile failed");
    result(*owner, future);
    auto horizon = command(5, 2 * owner->config.rate + 1, ES_RT_MASTER);
    submit(*owner, horizon, ES_RT_INVALID);
    --horizon.frame;
    submit(*owner, horizon);

    auto exhausted = create(44100);
    fleet::Scheduler near(*exhausted, nullptr, nullptr, UINT64_MAX - 127);
    require(near.renderTile(tile) == ES_RT_OK && near.generated() == UINT64_MAX - 63 &&
        exhausted->commands.sealed == UINT64_MAX - 63, "Last complete quantum failed or wrapped");
    const auto prior = tile.first;
    require(near.renderTile(tile) == ES_RT_CAPACITY && tile.first == prior && near.generated() == UINT64_MAX - 63,
            "An exhausted timeline altered output or advanced");
    std::cout << "Post-snapshot arrival stayed pending through the sealed tile and was rejected late at the next tile.\n";
}

struct Audio { std::vector<float> pcm; es_telemetry state; };
constexpr std::size_t frames = 2048;
const std::vector<std::uint64_t> rawTargets{0, 7, 7, 63, 64, 65, 118, 255, 441, 512, 1023, 1024};

Audio fleetRender(const fs::path &model, unsigned rate, const es_rt_voice_config &settings,
                  const std::vector<std::size_t> &chunks) {
    auto owner = create(rate);
    const auto events = schedule(*owner, model, settings, 0, rawTargets);
    Driver driver(*owner);
    Audio output{std::vector<float>(frames), {}};
    for (std::size_t next = 0, offset = 0; offset < frames; ++next) {
        const auto extent = std::min(chunks[next % chunks.size()], frames - offset);
        driver.copy(output.pcm.data() + offset, extent);
        offset += extent;
    }
    require(driver.scheduler.generated() == frames, "Partition test delivered untimed output");
    for (const auto &value : events) result(*owner, value);
    output.state = owner->cells[events.front().candidate.cell].voice->runtime.telemetry();
    return output;
}

Audio rawRender(const fs::path &model, unsigned rate, const es_rt_voice_config &settings) {
    const es_config options{sizeof(options), ES_ABI_V2, rate, settings.mode == ES_RT_FREE ? ES_FREE : ES_DRIVEN,
                            settings.seed, settings.simulation_hz, 0, settings.starter_torque_nm};
    es_runtime *active = nullptr;
    require(es_create(&options, &active, nullptr) == ES_OK, "Could not create independent raw baseline");
    std::unique_ptr<es_runtime, decltype(&es_destroy)> raw(active, es_destroy);
    const auto entry = model.u8string(), root = model.parent_path().u8string();
    require(es_load_json_file(active, entry.c_str(), root.c_str(), nullptr, nullptr) == ES_OK, "Independent raw model failed");
    const auto set = [&](const es_rt_vector &target) {
        const es_controls controls{sizeof(es_controls), ES_ABI_V2, target.ignition, target.starter,
                                   target.demand, target.load_nm, target.driven_rpm};
        require(es_set_controls(active, &controls, nullptr) == ES_OK, "Independent v2 vector was refused");
    };
    Audio output{std::vector<float>(frames), {}};
    std::uint64_t position = 0;
    const auto renderTo = [&](std::uint64_t stop) {
        const auto size = static_cast<std::uint32_t>(stop - position);
        std::uint32_t written = 0;
        require(es_render(active, size, output.pcm.data() + position, size, &written, nullptr) == ES_OK && written == size,
                "Raw baseline failed to advance through the admitted event schedule");
        position = stop;
    };
    set(settings.initial);
    for (auto at : rawTargets) {
        renderTo(at);
        const es_rt_vector vector{at % 2 ? .35 : .53, at % 3 ? .25 : 1,
            settings.mode == ES_RT_DRIVEN && at >= 441 ? 120. : 0., 1, at > 500 ? 0u : settings.initial.starter};
        set(vector);
    }
    renderTo(frames);
    output.state.size = sizeof(output.state); output.state.abi = ES_ABI_V2;
    require(es_get_telemetry(active, &output.state, nullptr) == ES_OK, "Raw telemetry failed");
    return output;
}

void parity(const fs::path &model) {
    for (auto rate : {44100u, 48000u}) {
        for (bool driven : {false, true}) {
            const auto settings = voice(14321, driven);
            const auto expected = rawRender(model, rate, settings);
            for (const auto &chunks : {std::vector<std::size_t>{1}, {7, 251, 31, 1, 503}, {256}}) {
                const auto actual = fleetRender(model, rate, settings, chunks);
                require(std::memcmp(actual.pcm.data(), expected.pcm.data(), frames * sizeof(float)) == 0 &&
                        std::memcmp(&actual.state, &expected.state, sizeof(actual.state)) == 0,
                        "RAW scheduler was not partition-invariant or diverged from independent v2 control timeline");
            }
            std::cout << "Exact raw float PCM and telemetry across three delivery partitions, rate=" << rate
                      << " driven=" << driven << " frames=" << frames << " ticks=" << expected.state.ticks << '\n';
        }
    }
}
}

int main(int argc, char **argv) try {
    require(argc == 3, "Expected scheduler suite and fixture");
    const auto file = fs::absolute(argv[2]);
    if (std::string_view(argv[1]) == "timeline") {
        for (auto rate : {44100u, 48000u})
            for (auto frequency : {12345u, rate, 90000u}) rationalWitness(file, rate, frequency);
        sealRace();
    } else parity(file);
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
