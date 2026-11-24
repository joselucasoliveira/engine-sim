#include "scheduler.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
namespace core = engine_sim::fleet;
using engine_sim::RuntimeTestAccess;
using namespace std::chrono_literals;
using Fleet = std::unique_ptr<es_rt_fleet, decltype(&es_rt_destroy)>;
using Raw = std::unique_ptr<es_runtime, decltype(&es_destroy)>;
constexpr unsigned rate = 44100, q = 64, total = 9216;
constexpr std::uint64_t epoch = 732;
constexpr unsigned swapAt = 4097, swapFrames = 4410, swapEnd = swapAt + swapFrames;
constexpr unsigned targetAt = swapAt + 2233, stopAt = 8833;
constexpr unsigned noFault = UINT32_MAX;
constexpr es_rt_vector a{.25, 0, 0, 1, 1}, b{.65, .25, 0, 1, 1}, c{.4, .375, 0, 1, 1};

void require(bool valid, const char *message) {
    if (!valid) throw std::runtime_error(message);
}
bool same(float left, float right) { return std::memcmp(&left, &right, sizeof(float)) == 0; }

Fleet owner(unsigned lanes = 2) {
    const es_rt_config config{sizeof(config), ES_RT_V1, rate, ES_RT_BALANCED, q, lanes, ES_RT_EXPERIMENTAL, 0, epoch};
    es_rt_fleet *value = nullptr;
    require(es_rt_create(&config, &value, nullptr) == ES_RT_OK && value, "new replacement epoch");
    return {value, es_rt_destroy};
}
std::int32_t prepare(es_rt_fleet &fleet, const fs::path &model, unsigned seed, es_rt_vector first, es_rt_token &out) {
    const es_rt_voice_config voice{sizeof(voice), ES_RT_V1, ES_RT_FREE, rate, ES_RT_RAW, 0, seed, 0, first};
    const auto path = model.u8string(), root = model.parent_path().u8string();
    return es_rt_prepare_file(&fleet, &voice, path.c_str(), root.c_str(), &out, nullptr);
}
es_rt_token voice(es_rt_fleet &fleet, const fs::path &model, unsigned seed, es_rt_vector first,
                  unsigned fail = noFault) {
    es_rt_token token{};
    require(prepare(fleet, model, seed, first, token) == ES_RT_OK, "prepare replacement voice");
    if (fail != noFault) {
        auto &runtime = fleet.cells[token.cell].voice->runtime;
        auto &shaft = RuntimeTestAccess::simulator(runtime).getEngine()->getOutputCrankshaft()->m_body;
        RuntimeTestAccess::pull(runtime).setTickCommands([&shaft, fail, tick = 0u]() mutable {
            if (tick++ == fail) shaft.theta = std::numeric_limits<double>::infinity();
        }, {});
    }
    return token;
}

es_rt_event event(unsigned kind, unsigned sequence, unsigned frame, unsigned lane,
                  unsigned generation, unsigned duration = 0) {
    es_rt_event result{};
    result.size = sizeof(result); result.abi = ES_RT_V1; result.epoch = epoch;
    result.kind = kind; result.sequence = sequence; result.frame = frame; result.duration_ms = duration;
    if (kind == ES_RT_ACTIVATE) result.next_voice = {generation, lane, 0};
    else result.voice = {generation, lane, 0};
    return result;
}
es_rt_event activate(es_rt_fleet &fleet, unsigned sequence, unsigned lane, es_rt_token token) {
    auto command = event(ES_RT_ACTIVATE, sequence, 0, lane, 1);
    command.candidate = token;
    require(es_rt_submit(&fleet, &command) == ES_RT_OK, "activate source");
    return command;
}
es_rt_event replace(unsigned sequence, unsigned at, unsigned generation, unsigned ms, es_rt_token token) {
    auto command = event(ES_RT_REPLACE, sequence, at, 0, generation, ms);
    command.next_voice = {generation + 1, 0, 0};
    command.candidate = token;
    return command;
}
void submit(es_rt_fleet &fleet, const es_rt_event &command, std::int32_t expected = ES_RT_OK) {
    const auto status = es_rt_submit(&fleet, &command);
    if (status != expected) std::cerr << "submit sequence " << command.sequence << " status " << status
                                     << " expected " << expected << '\n';
    require(status == expected, "swap/stop submission");
}

void result(es_rt_fleet &fleet, const es_rt_event &command, std::uint32_t reason,
            std::uint64_t applied, std::uint64_t end) {
    es_rt_result ticket{};
    ticket.size = sizeof(ticket); ticket.abi = ES_RT_V1;
    const auto status = es_rt_poll_result(&fleet, command.sequence, &ticket);
    if (status != ES_RT_OK || ticket.reason != reason || ticket.applied != applied || ticket.ended != end)
        std::cerr << "ticket seq=" << command.sequence << " status=" << status << " reason=" << ticket.reason
                  << " applied=" << ticket.applied << " ended=" << ticket.ended << '\n';
    require(status == ES_RT_OK && ticket.requested == command.frame && ticket.reason == reason &&
            ticket.applied == applied && ticket.ended == end && ticket.sequence == command.sequence &&
            ticket.next_voice.generation == command.next_voice.generation &&
            ticket.voice.generation == command.voice.generation, "terminal swap/fault ticket");
}
es_rt_transport position(es_rt_fleet &fleet) {
    es_rt_transport p{};
    p.size = sizeof(p); p.abi = ES_RT_V1;
    require(es_rt_get_transport(&fleet, &p) == ES_RT_OK, "replacement position");
    return p;
}
void start(es_rt_fleet &fleet) { require(es_rt_start(&fleet, nullptr) == ES_RT_OK, "prime replacement epoch"); }
void join(es_rt_fleet &fleet) { require(es_rt_quiesce(&fleet) == ES_RT_OK, "join replacement epoch"); }

void request(es_runtime &raw, es_rt_vector value) {
    const es_controls control{sizeof(control), ES_ABI_V2, value.ignition, value.starter, value.demand, value.load_nm, value.driven_rpm};
    es_error issue{};
    require(es_set_controls(&raw, &control, &issue) == ES_OK, issue.message);
}
std::vector<float> isolate(const fs::path &model, unsigned seed, es_rt_vector first,
                           unsigned frames, unsigned change = noFault) {
    es_config config{};
    es_error issue{};
    require(es_config_init(ES_ABI_V2, sizeof(config), &config, &issue) == ES_OK, issue.message);
    config.output_hz = rate;
    config.simulation_hz = rate;
    config.seed = seed;
    es_runtime *raw = nullptr;
    require(es_create(&config, &raw, &issue) == ES_OK, issue.message);
    Raw runtime(raw, es_destroy);
    const auto entry = model.u8string(), root = model.parent_path().u8string();
    require(es_load_json_file(raw, entry.c_str(), root.c_str(), nullptr, &issue) == ES_OK, issue.message);
    request(*raw, first);
    std::vector<float> data(frames);
    auto segment = [&](unsigned i, unsigned count) {
        std::uint32_t written = 0;
        require(es_render(raw, count, data.data() + i, count, &written, &issue) == ES_OK && written == count, issue.message);
    };
    segment(0, std::min(change, frames));
    if (change < frames) {
        request(*raw, c);
        segment(change, frames - change);
    }
    return data;
}
double signal(const std::vector<float> &healthy, unsigned at, unsigned fault) {
    if (at < fault) return healthy[at];
    if (!fault || at - fault >= 31) return 0.;
    return static_cast<double>(healthy[fault - 1]) * (1. - static_cast<double>(at - fault + 1) / 32.);
}
double fade(unsigned at, unsigned first, unsigned length) {
    if (at >= first + length) return 1.;
    if (at <= first) return 0.;
    return static_cast<double>(at - first) / length;
}

struct Pull {
    es_rt_fleet &fleet;
    es_rt_snapshot latest{};
    unsigned at = 0;
    void next(unsigned frames, const std::function<void(unsigned, float, float, float)> &inspect) {
        latest.size = sizeof(latest); latest.abi = ES_RT_V1;
        const auto until = std::chrono::steady_clock::now() + 8s;
        for (;;) {
            while (es_rt_poll_snapshot(&fleet, &latest) == ES_RT_OK) {}
            if (latest.generated >= at + frames) break;
            require(std::chrono::steady_clock::now() < until, "replacement producer stalled");
            std::this_thread::sleep_for(100us);
        }
        std::array<float, q + 2> active{}, other{}, mix{};
        active.fill(52.f); other.fill(52.f); mix.fill(52.f);
        es_rt_audio audio{sizeof(audio), ES_RT_V1, frames, q, mix.data() + 1, {active.data() + 1,
                          fleet.config.lanes > 1 ? other.data() + 1 : nullptr}};
        es_rt_block result{sizeof(result), ES_RT_V1, 0, 0, 0, 0};
        require(es_rt_process(&fleet, &audio, &result) == ES_RT_OK && result.first == at &&
                result.pcm_frames == frames && !result.fallback_frames, "replacement broke PCM transport");
        for (unsigned i = 0; i < frames; ++i) inspect(at + i, active[i+1], other[i+1], mix[i+1]);
        require(active.front() == 52.f && other.front() == 52.f && mix.front() == 52.f &&
                active[frames+1] == 52.f && other[frames+1] == 52.f && mix[frames+1] == 52.f, "replacement plane extent");
        at += frames;
    }
    void through(unsigned end) {
        while (at < end) next(std::min(q, end - at), [](unsigned, float, float, float) {});
    }
};

void archived(const char *label, const std::vector<float> &samples, const char *part) {
    const auto *root = std::getenv("ES_FLEET_EVIDENCE");
    if (!root || !*root) return;
    std::ofstream out(fs::u8path(root) / (std::string(label) + "-" + part + ".f32le"), std::ios::binary);
    out.write(reinterpret_cast<const char *>(samples.data()), static_cast<std::streamsize>(samples.size() * sizeof(float)));
    require(static_cast<bool>(out), "capture replacement evidence");
}

struct Reference {
    std::vector<float> before, after, witness;
};
struct Trial {
    const char *label;
    unsigned oldFault, newFault;
    unsigned fadeMs = 100;
    bool zeroGain = false;
};

void overlap(const fs::path &model, const Reference &ref, Trial trial) {
    const auto fadeLength = trial.fadeMs ? swapFrames : 0;
    const auto endpoint = swapAt + fadeLength;
    auto fleet = owner();
    const auto old = voice(*fleet, model, 403, a, trial.oldFault);
    const auto other = voice(*fleet, model, 419, a);
    auto startup1 = activate(*fleet, 1, 0, old);
    auto startup2 = activate(*fleet, 2, 1, other);
    start(*fleet);
    result(*fleet, startup1, ES_RT_OK, 0, 0);
    result(*fleet, startup2, ES_RT_OK, 0, 0);
    const auto fresh = voice(*fleet, model, 411, b, trial.newFault);
    const auto spare = voice(*fleet, model, 420, b);
    unsigned seq = 3;
    es_rt_event zero{};
    if (trial.zeroGain) {
        zero = event(ES_RT_GAIN, seq++, 3000, 0, 1);
        zero.gain = 0.;
        submit(*fleet, zero);
    }
    auto swap = replace(seq++, swapAt, 1, trial.fadeMs, fresh);
    submit(*fleet, swap);
    auto stacked = replace(seq, swapAt + 1, 1, 0, spare);
    stacked.next_voice.generation = 3;
    submit(*fleet, stacked, ES_RT_BUSY);
    require(es_rt_discard(fleet.get(), spare) == ES_RT_OK, "refused overlap transferred a second token");

    es_rt_event busy{};
    if (fadeLength) {
        busy = event(ES_RT_STOP, seq++, swapAt + 101, 0, 2);
        submit(*fleet, busy);
    }
    auto target = event(ES_RT_VECTOR, seq++, targetAt, 0, 2);
    target.physical = c;
    submit(*fleet, target);
    auto stopNew = event(ES_RT_STOP, seq++, stopAt, 0, 2);
    auto stopOther = event(ES_RT_STOP, seq++, stopAt, 1, 1);
    submit(*fleet, stopNew); submit(*fleet, stopOther);

    Pull pull{*fleet};
    std::vector<float> expected, actual, referenceMix, actualMix;
    unsigned oldTail = 0, newTail = 0;
    bool sawOverlap = false;
    constexpr unsigned sizes[] = {63, 1, 64, 33};
    for (unsigned turn = 0; pull.at < total; ++turn) {
        pull.next(std::min(total - pull.at, sizes[turn % std::size(sizes)]),
            [&](unsigned frame, float actualLane, float witness, float mix) {
            double destination = 0.;
            if (frame < swapAt) destination = signal(ref.before, frame, trial.oldFault);
            else if (frame < stopAt) {
                const auto u = fade(frame, swapAt, fadeLength);
                const auto after = signal(ref.after, frame - swapAt, trial.newFault);
                if (u == 1.) destination = after;
                else {
                    const auto before = signal(ref.before, frame, trial.oldFault);
                    destination = (1. - u) * before + u * after;
                    if (frame >= trial.oldFault && before != 0.) ++oldTail;
                }
                if (frame - swapAt >= trial.newFault && after != 0.) ++newTail;
            }
            const auto lane = trial.zeroGain && frame >= 3000 ? 0.f : static_cast<float>(destination);
            const auto otherLane = frame < stopAt ? ref.witness[frame] : 0.f;
            const auto bus = static_cast<float>((0. + static_cast<double>(lane) + static_cast<double>(otherLane)) * .5);
            if (!same(actualLane, lane) || !same(witness, otherLane) || !same(mix, bus)) {
                std::cerr << trial.label << " frame=" << frame << std::hexfloat << " have=" << actualLane
                          << " want=" << lane << " other=" << witness << " other-ref=" << otherLane
                          << " mix=" << mix << " mix-ref=" << bus << std::defaultfloat << '\n';
                throw std::runtime_error("overlap or unaffected lane PCM");
            }
            expected.push_back(lane); actual.push_back(actualLane);
            referenceMix.push_back(bus); actualMix.push_back(mix);
        });
        if (fadeLength && !sawOverlap && pull.at >= swapAt + q) {
            const auto &latest = pull.latest.lane[0];
            require(latest.voice.generation == 2 && pull.latest.generated < endpoint &&
                    position(*fleet).returned_cells == 0, "overlap released old cell before endpoint");
            auto oldRequest = event(ES_RT_VECTOR, seq, stopAt, 0, 1);
            oldRequest.physical = c;
            submit(*fleet, oldRequest, ES_RT_STALE);
            sawOverlap = true;
        }
    }
    join(*fleet);
    require(!fadeLength || sawOverlap, "overlap setup did not reach an observed in-flight boundary");
    const bool failed = trial.oldFault != noFault || trial.newFault != noFault;
    if (trial.zeroGain) result(*fleet, zero, ES_RT_OK, 3000, 3000);
    result(*fleet, swap, failed ? ES_RT_FAULT : ES_RT_OK, swapAt, endpoint);
    if (fadeLength) result(*fleet, busy, ES_RT_BUSY, ES_RT_NO_FRAME, ES_RT_NO_FRAME);
    if (trial.newFault <= targetAt - swapAt) result(*fleet, target, ES_RT_STATE, ES_RT_NO_FRAME, ES_RT_NO_FRAME);
    else result(*fleet, target, ES_RT_OK, targetAt, targetAt);
    result(*fleet, stopNew, ES_RT_OK, stopAt, stopAt);
    result(*fleet, stopOther, ES_RT_OK, stopAt, stopAt);

    std::array<bool, 3> seen{};
    es_rt_token token{};
    es_rt_lane_info state{};
    while (es_rt_collect(fleet.get(), &token, &state) == ES_RT_OK) {
        unsigned i = token.cell == old.cell ? 0u : token.cell == fresh.cell ? 1u : 2u;
        require(!seen[i] && (i != 2 || token.cell == other.cell), "duplicated or unknown returned generation");
        seen[i] = true;
        const auto limit = i == 0 ? std::min(endpoint, trial.oldFault) :
                           i == 1 ? std::min(stopAt - swapAt, trial.newFault) : stopAt;
        const auto numericalFault = (i == 0 && trial.oldFault != noFault) || (i == 1 && trial.newFault != noFault);
        require(state.frames == limit && state.ticks == limit && state.native_frames == limit &&
                state.voice.generation == (i == 1 ? 2u : 1u) &&
                state.fault == (numericalFault ? ES_RT_FAULT : ES_RT_OK), "returned overlap prefix or fault identity");
    }
    require(std::all_of(seen.begin(), seen.end(), [](bool v) { return v; }), "lost overlap return");
    if (trial.oldFault != noFault) require(oldTail != 0, "old fault case has no visible emergency tail");
    if (trial.newFault != noFault && trial.newFault) require(newTail != 0, "new fault case has no visible emergency tail");
    archived(trial.label, expected, "stem-oracle"); archived(trial.label, actual, "stem");
    archived(trial.label, referenceMix, "mix-oracle"); archived(trial.label, actualMix, "mix");
    std::cout << trial.label << " s=" << swapAt << " end=" << endpoint << " terminal="
              << (failed ? ES_RT_FAULT : ES_RT_OK) << " old-tail-nonzero=" << oldTail
              << " new-tail-nonzero=" << newTail << " unaffected-frames=" << stopAt << '\n';
}

void ordinary(const fs::path &model, const Reference &ref, bool pause) {
    constexpr unsigned at = 4097, failure = at + 7, final = at + 12, duration = 45, end = final + duration;
    auto fleet = owner();
    auto one = activate(*fleet, 1, 0, voice(*fleet, model, 403, a, failure));
    auto two = activate(*fleet, 2, 1, voice(*fleet, model, 419, a));
    auto first = event(pause ? ES_RT_PAUSE : ES_RT_STOP, 3, at, 0, 1, 2);
    auto force = event(ES_RT_STOP, 4, final, 0, 1, 1);
    submit(*fleet, first); submit(*fleet, force);
    start(*fleet);
    const double held = 1. - fade(failure, at, 89);
    const double frozen = signal(ref.before, final, failure);
    require(frozen != 0., "forced stop residual is silent");
    Pull pull{*fleet};
    while (pull.at < 4352) {
        pull.next(q, [&](unsigned x, float lane, float other, float mix) {
            double source = 0., gate = 0.;
            if (x < failure) { source = ref.before[x]; gate = 1. - fade(x, at, 89); }
            else if (x < final) { source = signal(ref.before, x, failure); gate = held; }
            else if (x < end) { source = frozen; gate = held + (0. - held) * fade(x, final, duration); }
            const auto scaled = gate == 0 ? 0.f : static_cast<float>(source * gate);
            const auto witness = ref.witness[x];
            require(same(scaled, lane) && same(witness, other) &&
                    same(mix, static_cast<float>((static_cast<double>(scaled) + witness) * .5)), "fault during lifecycle");
        });
    }
    join(*fleet);
    result(*fleet, one, ES_RT_OK, 0, 0); result(*fleet, two, ES_RT_OK, 0, 0);
    result(*fleet, first, ES_RT_FAULT, at, failure);
    result(*fleet, force, ES_RT_OK, final, end);
    es_rt_token old{};
    es_rt_lane_info info{};
    require(es_rt_collect(fleet.get(), &old, &info) == ES_RT_OK && info.frames == failure &&
            info.fault == ES_RT_FAULT, "forced stop advanced failed physics or lost fault status");
    std::cout << (pause ? "fault-during-pause" : "fault-during-stop") << " f=" << failure
              << " superseding-stop=" << final << " end=" << end << " untruncated-source=" << frozen << '\n';
}

void cancellation(const fs::path &model) {
    for (bool started : {false, true}) {
        auto fleet = owner(1);
        auto old = voice(*fleet, model, 403, a);
        activate(*fleet, 1, 0, old);
        const auto next = voice(*fleet, model, 411, b);
        const auto swap = replace(2, 1024, 1, 100, next);
        submit(*fleet, swap);
        start(*fleet);
        if (started) {
            Pull pull{*fleet};
            pull.through(1216);
        }
        join(*fleet);
        result(*fleet, swap, ES_RT_CANCELED, started ? 1024u : ES_RT_NO_FRAME, ES_RT_NO_FRAME);
        es_rt_token token{};
        es_rt_lane_info state{};
        unsigned seen = 0;
        while (es_rt_collect(fleet.get(), &token, &state) == ES_RT_OK) {
            if (token.cell == old.cell) seen |= 1;
            else if (token.cell == next.cell) seen |= 2;
        }
        require(seen == 3, "quiesce lost a queued or overlapping cell");
    }
}

void late(const fs::path &model) {
    auto fleet = owner(1);
    auto old = voice(*fleet, model, 403, a);
    auto first = activate(*fleet, 1, 0, old);
    const auto next = voice(*fleet, model, 411, b);
    auto swap = replace(2, 96, 1, 100, next);
    struct Submit { es_rt_fleet *fleet; es_rt_event &swap; int status = -1; } body{fleet.get(), swap};
    core::Scheduler scheduler(*fleet);
    core::ScheduledTile tile;
    require(scheduler.renderTile(tile) == ES_RT_OK, "drive exact first private tile");
    result(*fleet, first, ES_RT_OK, 0, 0);
    const auto after = [](void *context) noexcept {
        auto &p = *static_cast<Submit *>(context);
        p.status = es_rt_submit(p.fleet, &p.swap);
    };
    require(scheduler.renderTile(tile, after, &body) == ES_RT_OK && body.status == ES_RT_OK &&
            scheduler.renderTile(tile) == ES_RT_OK, "publish replacement just after seal snapshot");
    result(*fleet, swap, ES_RT_LATE, ES_RT_NO_FRAME, ES_RT_NO_FRAME);
    es_rt_token returned{};
    es_rt_lane_info state{};
    require(es_rt_collect(fleet.get(), &returned, &state) == ES_RT_OK && returned.cell == next.cell &&
            state.voice.generation == 2 && state.frames == 0 && scheduler.lane(0).id.generation == 1,
            "late replacement orphaned new cell or displaced old");
}

void backlog(const fs::path &model) {
    auto fleet = owner(1);
    auto startup = activate(*fleet, 1, 0, voice(*fleet, model, 403, a));
    start(*fleet);
    result(*fleet, startup, ES_RT_OK, 0, 0);
    Pull pull{*fleet};
    for (unsigned i = 1; i <= 3; ++i) {
        const auto next = voice(*fleet, model, 410 + i, b);
        const unsigned at = pull.at + position(*fleet).high_frames + q;
        const auto swap = replace(i + 1, at, i, 0, next);
        submit(*fleet, swap);
        pull.through(at + q);
        result(*fleet, swap, ES_RT_OK, at, at);
    }
    auto guard = es_rt_token{9, 7, 5, 0};
    require(position(*fleet).returned_cells == 3 && prepare(*fleet, model, 420, b, guard) == ES_RT_CAPACITY &&
            guard.epoch == 9, "full returned slots permits over-capacity replacement");
    es_rt_lane_info state{};
    require(es_rt_collect(fleet.get(), &guard, &state) == ES_RT_OK && state.voice.generation == 1, "retire backlog head");
    const auto recycled = voice(*fleet, model, 421, b);
    require(recycled.cell == guard.cell && recycled.generation == guard.generation + 1, "stale replacement reuses cell");
    require(es_rt_discard(fleet.get(), recycled) == ES_RT_OK, "discard recycled spare");
    join(*fleet);
    std::cout << "returned-slot-backlog=3 retained-active=1 zero-duration-swaps=3\n";
}

void synthetic() {
    for (unsigned i = 0; i <= 64; ++i) {
        const double w = i / 64.;
        require(core::crossfade(.375, .375, w) == .375, "correlated unity");
        require(core::crossfade(.5, -.5, w) == .5 - w, "anti-phase complement");
    }
    for (unsigned i = 0; i < 96; ++i)
        require(core::faultSample(-.5f, i) == (i < 31 ? -.5 * (31-i) / 32. : 0.), "exact 32 frame fault fade");
}
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    try {
        const auto model = fs::absolute(argv[2]);
        const Reference ref{isolate(model, 403, a, total), isolate(model, 411, b, total, targetAt - swapAt),
                            isolate(model, 419, a, total)};
        const std::string_view mode = argv[1];
        if (mode == "replace") {
            synthetic();
            overlap(model, ref, {"ordinary", noFault, noFault});
            overlap(model, ref, {"immediate", noFault, noFault, 0});
            overlap(model, ref, {"silent-gain", noFault, noFault, 100, true});
            cancellation(model); late(model); backlog(model);
        } else if (mode == "fault") {
            for (const auto &trial : std::array<Trial, 8>{{
                {"old-first", swapAt, noFault}, {"old-middle", swapAt + 2205, noFault},
                {"old-last", swapEnd - 1, noFault}, {"new-first", noFault, 0},
                {"new-middle", noFault, 2205}, {"new-last", noFault, swapFrames - 1},
                {"both", swapAt + 3200, 2205}, {"zero-gain-fault", swapAt + 2205, 2205, 100, true}
            }}) overlap(model, ref, trial);
            ordinary(model, ref, false);
            ordinary(model, ref, true);
        } else return 2;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
