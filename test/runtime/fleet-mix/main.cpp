#include "mix.hpp"
#include "owner.hpp"

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

namespace {
namespace fs = std::filesystem;
namespace core = engine_sim::fleet;
using namespace std::chrono_literals;
using Owner = std::unique_ptr<es_rt_fleet, decltype(&es_rt_destroy)>;
using Raw = std::unique_ptr<es_runtime, decltype(&es_destroy)>;
constexpr unsigned rate = 48000, q = 64, maximum = 128, total = 18432;
constexpr std::uint64_t epoch = 542;

struct Model {
    fs::path path, root;
    bool driven;
    unsigned seed;
    es_rt_vector initial;
};
struct Figure {
    std::vector<float> pcm;
    std::vector<es_telemetry> snapshots;
};

void require(bool ok, const char *failure) {
    if (!ok) throw std::runtime_error(failure);
}
template<class T>
bool identical(T a, T b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

es_rt_config config(unsigned lanes) {
    return {sizeof(es_rt_config), ES_RT_V1, rate, ES_RT_BALANCED, maximum, lanes, 0, 0, epoch};
}
Owner create(unsigned lanes) {
    auto options = config(lanes);
    es_rt_fleet *fleet = nullptr;
    require(es_rt_create(&options, &fleet, nullptr) == ES_RT_OK && fleet, "fleet roster create");
    return {fleet, es_rt_destroy};
}

std::int32_t prepare(es_rt_fleet &fleet, const Model &model, es_rt_token &token) {
    const es_rt_voice_config options{sizeof(options), ES_RT_V1, model.driven ? ES_RT_DRIVEN : ES_RT_FREE,
                                     0, ES_RT_RAW, 0, model.seed, 0, model.initial};
    const auto path = model.path.u8string(), root = model.root.u8string();
    return es_rt_prepare_file(&fleet, &options, path.c_str(), root.c_str(), &token, nullptr);
}
es_rt_event event(std::uint32_t kind, std::uint64_t frame, std::uint32_t lane) {
    es_rt_event result{};
    result.size = sizeof(result);
    result.abi = ES_RT_V1;
    result.kind = kind;
    result.epoch = epoch;
    result.frame = frame;
    if (kind == ES_RT_ACTIVATE) result.next_voice = {1, lane, 0};
    else if (kind != ES_RT_MASTER) result.voice = {1, lane, 0};
    return result;
}

void submit(es_rt_fleet &fleet, es_rt_event command, std::uint64_t sequence, std::int32_t status = ES_RT_OK) {
    command.sequence = sequence;
    require(es_rt_submit(&fleet, &command) == status, "submit roster event");
}
void receipt(es_rt_fleet &fleet, std::uint64_t sequence, const es_rt_event &command) {
    es_rt_result result{};
    result.size = sizeof(result);
    result.abi = ES_RT_V1;
    require(es_rt_poll_result(&fleet, sequence, &result) == ES_RT_OK && result.reason == ES_RT_OK &&
            result.epoch == epoch && result.kind == command.kind && result.requested == command.frame &&
            result.applied == command.frame && result.ended == command.frame, "roster ticket");
}
es_rt_transport transport(es_rt_fleet &fleet) {
    es_rt_transport result{};
    result.size = sizeof(result);
    result.abi = ES_RT_V1;
    require(es_rt_get_transport(&fleet, &result) == ES_RT_OK, "roster transport");
    return result;
}

void checkLedger(const es_rt_fleet &fleet, const char *stage, unsigned count) {
    auto sum = fleet.fixedBytes;
    unsigned cells = 0;
    for (std::size_t i = 0; i < fleet.cellCount; ++i) {
        if (!fleet.cells[i].voice) continue;
        const auto bytes = fleet.cells[i].voice->capacity.retained_bytes;
        require(bytes && bytes <= engine_sim::assembly::fleet_cell_limit, "voice outside cell byte limit");
        sum += bytes;
        ++cells;
    }
    require(cells == count && sum == fleet.retainedBytes && fleet.fixedBytes <= engine_sim::assembly::fleet_storage_limit &&
            fleet.cellCount == fleet.config.lanes + 3, "fixed and retained fleet accounting mismatch");
    std::cout << stage << " V=" << fleet.config.lanes << " cells=" << fleet.cellCount << " voices=" << count
              << " fixed-bytes=" << fleet.fixedBytes << " retained-bytes=" << sum << '\n';
}

es_rt_vector nextControls(const Model &model, unsigned index) {
    auto result = model.initial;
    if (index >= 1) {
        result.driven_rpm = model.driven ? 600 : 0;
        result.demand = .48;
    }
    if (index >= 2) {
        result.demand = .61;
        result.starter = 0;
        result.load_nm = .25;
    }
    return result;
}
constexpr unsigned eventsAt[] = {0, 4800, 16000};

Figure isolated(const Model &model) {
    es_config rawConfig{};
    es_error detail{};
    require(es_config_init(ES_ABI_V2, sizeof(rawConfig), &rawConfig, &detail) == ES_OK, detail.message);
    rawConfig.output_hz = rate;
    rawConfig.mode = model.driven ? ES_DRIVEN : ES_FREE;
    rawConfig.seed = model.seed;
    es_runtime *raw = nullptr;
    require(es_create(&rawConfig, &raw, &detail) == ES_OK, detail.message);
    Raw owner(raw, es_destroy);
    const auto path = model.path.u8string(), root = model.root.u8string();
    require(es_load_json_file(raw, path.c_str(), root.c_str(), nullptr, &detail) == ES_OK, detail.message);
    Figure figure{std::vector<float>(total), std::vector<es_telemetry>(total / q)};
    unsigned index = 0;
    for (unsigned i = 0; i < total; ++i) {
        if (index < std::size(eventsAt) && i == eventsAt[index]) {
            const auto u = nextControls(model, index++);
            const es_controls controls{sizeof(controls), ES_ABI_V2, u.ignition, u.starter, u.demand, u.load_nm, u.driven_rpm};
            require(es_set_controls(raw, &controls, &detail) == ES_OK, detail.message);
        }
        std::uint32_t written = 0;
        require(es_render(raw, 1, figure.pcm.data() + i, 1, &written, &detail) == ES_OK && written == 1, detail.message);
        if ((i + 1) % q == 0) {
            auto &snapshot = figure.snapshots[i / q];
            snapshot.size = sizeof(snapshot); snapshot.abi = ES_ABI_V2;
            require(es_get_telemetry(raw, &snapshot, &detail) == ES_OK, detail.message);
        }
    }
    return figure;
}

struct Reader {
    es_rt_fleet &fleet;
    unsigned lanes, submitted = 0;
    std::uint64_t observed = 0;
    es_rt_snapshot latest{};

    template<class CheckSnapshot, class CheckAudio>
    void read(unsigned frames, CheckSnapshot checkSnapshot, CheckAudio checkAudio) {
        latest.size = sizeof(latest);
        latest.abi = ES_RT_V1;
        const auto stop = std::chrono::steady_clock::now() + 8s;
        for (;;) {
            while (es_rt_poll_snapshot(&fleet, &latest) == ES_RT_OK) {
                require(latest.generated == observed + q && latest.dropped == 0 && latest.lanes == lanes, "telemetry skipped a mix tile");
                observed = latest.generated;
                checkSnapshot(latest);
            }
            if (observed >= submitted + frames) break;
            require(std::chrono::steady_clock::now() < stop, "mix transport stalled");
            std::this_thread::sleep_for(100us);
        }
        std::array<std::array<float, maximum + 2>, 9> data;
        for (auto &plane : data) plane.fill(13.f);
        es_rt_audio audio{sizeof(audio), ES_RT_V1, frames, maximum, data[0].data() + 1, {}};
        for (unsigned i = 0; i < lanes; ++i) audio.stem[i] = data[i + 1].data() + 1;
        es_rt_block result{sizeof(result), ES_RT_V1, 0, 0, 0, 0};
        require(es_rt_process(&fleet, &audio, &result) == ES_RT_OK && result.first == submitted &&
                result.fallback_frames == 0 && result.pcm_frames == frames, "fleet mix callback");
        for (unsigned i = 0; i <= lanes; ++i)
            require(data[i].front() == 13.f && data[i][frames + 1] == 13.f, "plane overrun");
        checkAudio(data, submitted, frames);
        submitted += frames;
    }
};

double projected(double from, double to, unsigned at, unsigned start, unsigned length) {
    if (at < start) return from;
    if (at >= start + length) return to;
    return from + (to - from) * (static_cast<double>(at - start) / length);
}

void record(const std::string &label, const std::vector<float> &pcm) {
    const char *directory = std::getenv("ES_FLEET_EVIDENCE");
    if (!directory || !*directory) return;
    std::ofstream out(fs::u8path(directory) / (label + ".f32le"), std::ios::binary);
    out.write(reinterpret_cast<const char *>(pcm.data()), static_cast<std::streamsize>(pcm.size() * sizeof(float)));
    require(static_cast<bool>(out), "write local mix evidence");
}

struct Delivered { std::vector<float> single, kohler, mix; };
Delivered pair(const Model &single, const Model &kohler, const Figure &left, const Figure &right,
               bool reversed, bool solo) {
    auto fleet = create(3);
    checkLedger(*fleet, "create", 0);
    es_rt_token a{}, b{};
    require(prepare(*fleet, single, a) == ES_RT_OK, "prepare FREE");
    if (!solo) require(prepare(*fleet, kohler, b) == ES_RT_OK, "prepare DRIVEN");
    auto first = event(ES_RT_ACTIVATE, 0, 0); first.candidate = a;
    auto second = event(ES_RT_ACTIVATE, 0, 2); second.candidate = b;
    std::vector<es_rt_event> scheduled;
    if (!solo && reversed) scheduled.push_back(second);
    scheduled.push_back(first);
    if (!solo && !reversed) scheduled.push_back(second);
    auto vector = [&](const Model &model, unsigned index, unsigned lane) {
        auto command = event(ES_RT_VECTOR, eventsAt[index], lane);
        command.physical = nextControls(model, index);
        scheduled.push_back(command);
    };
    vector(single, 1, 0);
    if (!solo) vector(kohler, 1, 2);
    auto master = event(ES_RT_MASTER, 9009, 0); master.gain = .75; master.duration_ms = ES_RT_DFLT_MS;
    scheduled.push_back(master);
    auto gainLeft = event(ES_RT_GAIN, 12291, 0); gainLeft.gain = .625;
    scheduled.push_back(gainLeft);
    if (!solo) {
        auto gainRight = event(ES_RT_GAIN, 12291, 2); gainRight.gain = .75; gainRight.duration_ms = 3;
        scheduled.push_back(gainRight);
    }
    vector(single, 2, 0);
    if (!solo) vector(kohler, 2, 2);
    for (std::size_t i = 0; i < scheduled.size(); ++i) submit(*fleet, scheduled[i], i + 1);
    require(es_rt_start(fleet.get(), nullptr) == ES_RT_OK, "prime mixed fleet");
    const std::size_t activations = solo ? 1 : 2;
    for (std::size_t i = 0; i < activations; ++i) receipt(*fleet, i + 1, scheduled[i]);
    checkLedger(*fleet, "prime", solo ? 1 : 2);

    Reader reader{*fleet, 3};
    Delivered output, oracle;
    std::uint64_t checked = 0;
    auto checkSnapshot = [&](const es_rt_snapshot &snapshot) {
        if (snapshot.generated > total) return;
        const auto compare = [&](unsigned l, const Figure &isolated) {
            const auto &voice = snapshot.lane[l];
            const auto &state = isolated.snapshots[snapshot.generated / q - 1];
            require(voice.state == ES_RT_PLAY && voice.voice.lane == l && voice.voice.generation == 1 &&
                voice.frames == state.frames && voice.ticks == state.ticks && voice.native_frames == state.native_frames &&
                identical(voice.measured_rpm, state.measured_rpm) && identical(voice.cycle_rad, state.cycle_rad) &&
                identical(voice.load_reaction_nm, state.load_reaction_nm) &&
                identical(voice.drive_reaction_nm, state.drive_reaction_nm), "isolated mechanical/clock comparison");
        };
        compare(0, left);
        if (!solo) compare(2, right);
        if (snapshot.generated >= 16064) {
            require(snapshot.lane[0].requested_frame == 16000 &&
                snapshot.lane[0].last_latched_sequence == snapshot.lane[0].last_sequence, "FREE scheduled target identity");
            if (!solo) require(snapshot.lane[2].ticks != snapshot.lane[0].ticks &&
                snapshot.lane[2].requested_frame == 16000 && snapshot.lane[2].applied.driven_rpm == 600,
                "DRIVEN roster control and independent clock");
        }
        require(snapshot.lane[1].state == ES_RT_VACANT, "empty lane state");
        ++checked;
    };
    const auto checkAudio = [&](const auto &data, unsigned from, unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            const auto f = from + i;
            const auto l = static_cast<float>(static_cast<double>(left.pcm[f]) * (f >= 12291 ? .625 : 1.));
            const auto r = solo ? 0.f : static_cast<float>(static_cast<double>(right.pcm[f]) * projected(1., .75, f, 12291, 144));
            const auto bus = static_cast<float>(std::clamp((0. + static_cast<double>(l) + 0. + static_cast<double>(r)) *
                                                           projected(.5, .75, f, 9009, 240), -1., 1.));
            require(identical(data[0][i+1], bus) && identical(data[1][i+1], l) &&
                identical(data[2][i+1], 0.f) && identical(data[3][i+1], r), "mixed PCM differs from isolated stems");
            output.single.push_back(data[1][i+1]); output.kohler.push_back(data[3][i+1]); output.mix.push_back(data[0][i+1]);
            oracle.single.push_back(l); oracle.kohler.push_back(r); oracle.mix.push_back(bus);
        }
    };
    bool rejected = false;
    const std::array<unsigned, 5> blocks{1u, 65u, 64u, 63u, maximum};
    for (unsigned turn = 0; reader.submitted < total; ++turn) {
        const auto size = reversed ? blocks[turn % blocks.size()] : blocks[blocks.size() - 1 - turn % blocks.size()];
        reader.read(std::min(total - reader.submitted, size), checkSnapshot, checkAudio);
        if (reader.submitted > 4096 && !rejected) {
            auto bad = single;
            bad.path = single.root / "nonexistent-roster-input.json";
            const auto before = fleet->retainedBytes;
            es_rt_token guard{9, 8, 7, 0};
            require(prepare(*fleet, bad, guard) == ES_RT_RESOURCE && guard.epoch == 9 &&
                    fleet->retainedBytes == before, "failed live preparation modified admitted fleet");
            rejected = true;
        }
    }
    require(rejected && checked == total / q, "missing mixed coverage");
    require(es_rt_quiesce(fleet.get()) == ES_RT_OK, "quiesce mixed fleet");
    checkLedger(*fleet, "quiesced", solo ? 1 : 2);
    for (std::size_t i = activations; i < scheduled.size(); ++i) receipt(*fleet, i + 1, scheduled[i]);
    es_rt_token returned{};
    es_rt_lane_info info{};
    unsigned collected = 0;
    while (es_rt_collect(fleet.get(), &returned, &info) == ES_RT_OK) ++collected;
    require(collected == (solo ? 1u : 2u), "return both mix contributors");
    checkLedger(*fleet, "collected", 0);

    const auto label = std::string(solo ? "solo" : (reversed ? "reverse" : "pair"));
    for (const auto &source : {std::make_pair("Single", &output.single), std::make_pair("CH750", &output.kohler)}) {
        const auto nonzero = std::count_if(source.second->begin(), source.second->end(), [](float f) { return f != 0; });
        require((solo && source.second == &output.kohler) ? nonzero == 0 : nonzero != 0, "silent stem witness");
        std::cout << label << " model=" << source.first << " written=" << total << " nonzero=" << nonzero << '\n';
    }
    require(std::memcmp(output.single.data(), left.pcm.data(), 12291 * sizeof(float)) == 0, "FREE RAW unity stem");
    if (!solo) require(std::memcmp(output.kohler.data(), right.pcm.data(), 12291 * sizeof(float)) == 0, "DRIVEN RAW unity stem");
    record(label + "-Single", output.single); record(label + "-CH750", output.kohler); record(label + "-mix", output.mix);
    record(label + "-Single-oracle", oracle.single); record(label + "-CH750-oracle", oracle.kohler);
    record(label + "-mix-oracle", oracle.mix);
    return output;
}

void maxRoster(const Model &single) {
    auto options = config(8);
    options.flags = 0;
    options.qualification_id = 1;
    es_rt_fleet *unmeasured = nullptr;
    require(es_rt_create(&options, &unmeasured, nullptr) == ES_RT_UNSUPPORTED && !unmeasured,
            "reserved Fleet qualification ID accepted");
    auto fleet = create(8);
    std::array<es_rt_event, 8> starts{};
    std::array<unsigned, 8> nonzero{};
    const auto offer = [&](unsigned l, std::uint64_t frame) {
        auto model = single;
        model.seed += l;
        auto activation = event(ES_RT_ACTIVATE, frame, l);
        require(prepare(*fleet, model, activation.candidate) == ES_RT_OK, "prepare bounded roster");
        submit(*fleet, activation, l + 1);
        starts[l] = activation;
    };
    offer(0, 0);
    offer(1, 0);
    require(es_rt_start(fleet.get(), nullptr) == ES_RT_OK, "prime roster maximum");
    receipt(*fleet, 1, starts[0]);
    receipt(*fleet, 2, starts[1]);
    Reader reader{*fleet, 8};
    const auto countSamples = [&](const auto &data, unsigned, unsigned count) {
        for (unsigned l = 0; l < nonzero.size(); ++l) {
            for (unsigned i = 1; i <= count; ++i) if (data[l+1][i] != 0.f) ++nonzero[l];
        }
    };
    for (unsigned l = 2; l < starts.size(); ++l) {
        const auto state = transport(*fleet);
        const auto at = state.submitted + state.high_frames + 2*q;
        offer(l, at);
        while (reader.submitted < at + q) reader.read(q, [](const es_rt_snapshot &) {}, countSamples);
        receipt(*fleet, l + 1, starts[l]);
        checkLedger(*fleet, "increasing", l + 1);
    }
    auto ninth = event(ES_RT_ACTIVATE, reader.submitted + 1024, 8);
    require(prepare(*fleet, single, ninth.candidate) == ES_RT_OK, "ninth prepared candidate");
    submit(*fleet, ninth, 9, ES_RT_INVALID);
    require(es_rt_discard(fleet.get(), ninth.candidate) == ES_RT_OK, "unadmitted voice was transferred");
    checkLedger(*fleet, "maximum", 8);
    const auto until = reader.submitted + 6304;
    while (reader.submitted < until) reader.read(q, [](const es_rt_snapshot &) {}, countSamples);
    require(es_rt_quiesce(fleet.get()) == ES_RT_OK, "join eight independent voices");
    for (unsigned l = 0; l < nonzero.size(); ++l) {
        require(nonzero[l] != 0 && reader.latest.lane[l].voice.generation == 1 &&
            reader.latest.lane[l].frames == reader.latest.generated - starts[l].frame, "bounded roster did not advance independently");
        std::cout << "lane=" << l << " seed=" << single.seed + l << " start=" << starts[l].frame
                  << " frames=" << reader.latest.lane[l].frames << " nonzero=" << nonzero[l] << '\n';
    }
    es_rt_token returned{};
    es_rt_lane_info info{};
    unsigned collected = 0;
    while (es_rt_collect(fleet.get(), &returned, &info) == ES_RT_OK) ++collected;
    require(collected == 8, "maximum roster return credits");
    checkLedger(*fleet, "maximum-collected", 0);
}

void ordered() {
    core::PcmTile values;
    values.mix.fill(-13.f);
    std::array<double, q> master{};
    master.fill(.5);
    values.stem[0][0] = .5f; values.stem[1][0] = .5f;
    values.stem[0][1] = .75f; values.stem[1][1] = -.75f;
    values.stem[0][2] = 1.f; values.stem[1][2] = 1.f; values.stem[2][2] = 1.f;
    values.stem[0][3] = -1.f; values.stem[1][3] = -1.f; values.stem[2][3] = -1.f;
    values.stem[0][4] = 1.f; values.stem[1][4] = std::ldexp(1.f, -53); values.stem[2][4] = -1.f;
    values.stem[0][5] = 1.f; values.stem[1][5] = -1.f; values.stem[2][5] = std::ldexp(1.f, -53);
    const auto stems = values.stem;
    core::writeMix(values, 3, master);
    const float expected[] = {.5f, 0.f, 1.f, -1.f, 0.f, std::ldexp(1.f, -54)};
    for (unsigned i = 0; i < std::size(expected); ++i)
        require(identical(values.mix[i], expected[i]), "ordered binary64 fold/clamp");
    for (unsigned i = std::size(expected); i < q; ++i) require(identical(values.mix[i], 0.f), "empty mix stale PCM");
    require(values.stem == stems, "mix rewrote a per-voice plane");
    std::cout << "mix synthetic cases=aligned,antiphase,clip-positive,clip-negative,ascending-fold,empty\n";
}
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    try {
        const auto source = fs::absolute(argv[2]);
        const Model single{source/"examples/runtime/fixture/main.json", source/"examples/runtime/fixture",
            false, 403, {.25, 0, 0, 1, 1}};
        const Model kohler{source/"assets/engines/atg-video-1/02_kohler_ch750.json", source,
            true, 404, {.25, 0, 0, 1, 0}};
        const std::string_view mode = argv[1];
        if (mode == "independent") {
            const auto left = isolated(single), right = isolated(kohler);
            const auto first = pair(single, kohler, left, right, false, false);
            const auto second = pair(single, kohler, left, right, true, false);
            const auto alone = pair(single, kohler, left, right, false, true);
            require(first.single == second.single && first.kohler == second.kohler && first.mix == second.mix &&
                    first.single == alone.single, "roster addition or activation order affected an existing stem");
            maxRoster(single);
        } else if (mode == "ordered") ordered();
        else return 2;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
