#include <engine_sim/fleet.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Frame = std::uint64_t;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr Frame block = 64;
constexpr std::uint64_t offsetBasis = UINT64_C(14695981039346656037);
constexpr std::uint64_t prime = UINT64_C(1099511628211);

void require(bool valid, const char *message) {
    if (!valid) throw std::runtime_error(message);
}
template<class T> T native() {
    T out{};
    out.size = sizeof(out);
    out.abi = ES_RT_V1;
    return out;
}
template<class T> T integer(const std::string &input) {
    T out{};
    const auto *end = input.data() + input.size();
    const auto parsed = std::from_chars(input.data(), end, out);
    require(parsed.ec == std::errc{} && parsed.ptr == end, "invalid replay integer");
    return out;
}
double real(const std::string &input) {
    std::size_t parsed = 0;
    const auto value = std::stod(input, &parsed);
    require(parsed == input.size() && std::isfinite(value), "invalid replay value");
    return value;
}

struct Control {
    Frame frame;
    es_rt_vector target;
};
struct Generation {
    Frame start, finish, overlap;
    std::uint64_t seed;
    std::vector<Control> controls;
};
struct Gain { Frame frame; double value; };
struct Timeline {
    std::vector<Generation> voices;
    std::vector<Gain> gains;
};

std::array<Timeline, 2> read(const char *path, Frame frames, std::uint32_t rate) {
    constexpr const char *fields = "sequence\tkind\tframe\ttrigger\tsampled\tD\tsubmit\treason\tapplied\tended\treaped\tlane\tvoice\tnext\tdemand\tload\trpm\tignition\tstarter\tgain";
    const auto attack = (5 * rate + 999) / 1000;
    const auto overlap = (20 * rate + 999) / 1000;
    std::ifstream file(path);
    std::string line;
    require(static_cast<bool>(std::getline(file, line)) && line == fields, "invalid replay event schema");
    std::array<Timeline, 2> lanes;
    Frame last = 0;
    std::uint64_t sequence = 0;
    std::uint64_t nextSeed = 405;
    while (std::getline(file, line)) {
        std::array<std::string, 20> column;
        std::size_t start = 0;
        for (std::size_t i = 0; i < column.size(); ++i) {
            const auto stop = line.find('\t', start);
            require((i + 1 == column.size()) == (stop == std::string::npos), "wrong replay event field count");
            column[i] = line.substr(start, stop == std::string::npos ? stop : stop - start);
            start = stop == std::string::npos ? stop : stop + 1;
        }
        const auto seq = integer<std::uint64_t>(column[0]);
        const auto kind = integer<unsigned>(column[1]);
        const auto frame = integer<Frame>(column[8]);
        const auto ended = integer<Frame>(column[9]);
        const auto lane = integer<unsigned>(column[11]);
        const auto voice = integer<std::uint64_t>(column[12]);
        const auto generation = integer<std::uint64_t>(column[13]);
        require(seq == ++sequence && frame >= last && frame < frames && lane < lanes.size() &&
                column[2] == column[8] && column[6] == "0" && column[7] == "0", "unaccepted or unordered replay event");
        last = frame;
        auto &state = lanes[lane];
        if (kind == ES_RT_ACTIVATE) {
            require(state.voices.empty() && frame == 0 && generation == 1 && !voice && ended == attack, "invalid replay activation");
            state.voices.push_back({frame, frames, 0, 403u + lane, {}});
            continue;
        }
        require(!state.voices.empty() && voice == state.voices.size(), "invalid replay generation");
        auto &current = state.voices.back();
        switch (kind) {
        case ES_RT_REPLACE:
            require(lane == 0 && generation == voice + 1 && frame >= current.start + current.overlap &&
                    ended == frame + overlap && ended <= frames, "invalid replay replacement endpoint");
            current.finish = ended;
            state.voices.push_back({frame, frames, overlap, nextSeed++, {}});
            break;
        case ES_RT_VECTOR:
            require(ended == frame && !generation, "invalid replay vector receipt");
            current.controls.push_back({frame - current.start, {real(column[14]), real(column[15]), real(column[16]),
                integer<std::uint32_t>(column[17]), integer<std::uint32_t>(column[18])}});
            break;
        case ES_RT_GAIN:
            require(ended == frame && !generation, "invalid replay gain receipt");
            state.gains.push_back({frame, real(column[19])});
            break;
        default:
            throw std::runtime_error("unsupported event in frozen replay");
        }
    }
    require(file.eof() && !lanes[0].voices.empty() && !lanes[1].voices.empty(), "missing replay lane");
    return lanes;
}

// Each generation has its own worker, starts at local frame zero and uses
// unity gain. The replay below applies the transition to the independent stems.
class Voice {
public:
    Voice(const Generation &voice, unsigned lane, std::uint32_t rate, const char *model, const char *root)
        : controls(voice.controls), lead(rate / 10) {
        auto config = native<es_rt_config>();
        config.rate = rate;
        config.profile = ES_RT_BALANCED;
        config.max_frames = block;
        config.lanes = 1;
        config.flags = ES_RT_EXPERIMENTAL;
        config.epoch = voice.seed;
        es_rt_fleet *handle = nullptr;
        auto diag = native<es_rt_diagnostic>();
        require(es_rt_create(&config, &handle, &diag) == ES_RT_OK, diag.message);
        fleet.reset(handle);
        auto cfg = native<es_rt_voice_config>();
        cfg.mode = lane == 0 ? ES_RT_FREE : ES_RT_DRIVEN;
        cfg.controls_policy = ES_RT_GAME;
        cfg.seed = voice.seed;
        cfg.initial = {.25, 0., 0., 1, lane == 0 ? 1u : 0u};
        auto on = native<es_rt_event>();
        on.epoch = config.epoch;
        epoch = config.epoch;
        on.sequence = 1;
        on.kind = ES_RT_ACTIVATE;
        on.next_voice = {1, 0, 0};
        require(es_rt_prepare_file(handle, &cfg, model, root, &on.candidate, &diag) == ES_RT_OK, diag.message);
        require(es_rt_submit(handle, &on) == ES_RT_OK, "replay activation submit");
        submit();
        require(es_rt_start(handle, &diag) == ES_RT_OK, diag.message);
    }

    void render(unsigned count, float *pcm) {
        submit();
        const auto until = Clock::now() + 3s;
        for (;;) {
            auto pos = native<es_rt_transport>();
            require(es_rt_get_transport(fleet.get(), &pos) == ES_RT_OK && !pos.broken && pos.submitted == cursor,
                    "replay transport position");
            if (pos.generated >= cursor + count) break;
            require(Clock::now() < until, "replay worker stalled");
            std::this_thread::sleep_for(50us);
        }
        auto request = native<es_rt_audio>();
        request.capacity = request.frames = count;
        request.stem[0] = pcm;
        auto result = native<es_rt_block>();
        require(es_rt_process(fleet.get(), &request, &result) == ES_RT_OK && result.first == cursor &&
                result.pcm_frames == count && !result.fallback_frames, "replay PCM discontinuity");
        cursor += count;
        auto snapshot = native<es_rt_snapshot>();
        while (es_rt_poll_snapshot(fleet.get(), &snapshot) == ES_RT_OK) {
            require(!snapshot.lane[0].fault, "replay engine fault");
        }
        reap();
    }

    void finish() {
        reap();
        require(next == controls.size() && received == next + 1, "missing replay receipt");
        require(es_rt_quiesce(fleet.get()) == ES_RT_OK, "replay teardown");
    }

private:
    void submit() {
        while (next < controls.size() && controls[next].frame <= cursor + lead) {
            auto event = native<es_rt_event>();
            event.kind = ES_RT_VECTOR;
            event.epoch = epoch;
            event.voice = {1, 0, 0};
            event.sequence = next + 2;
            event.frame = controls[next].frame;
            event.physical = controls[next].target;
            require(es_rt_submit(fleet.get(), &event) == ES_RT_OK, "replay vector submit");
            ++next;
        }
    }
    void reap() {
        while (received < next + 1) {
            auto result = native<es_rt_result>();
            const auto status = es_rt_poll_result(fleet.get(), received + 1, &result);
            if (status == ES_RT_PENDING) break;
            const auto expected = received == 0 ? 0 : controls[received - 1].frame;
            require(status == ES_RT_OK && result.reason == ES_RT_OK && result.applied == expected, "replay vector receipt");
            ++received;
        }
    }
    const std::vector<Control> &controls;
    const Frame lead;
    std::unique_ptr<es_rt_fleet, decltype(&es_rt_destroy)> fleet{nullptr, es_rt_destroy};
    Frame cursor = 0;
    std::uint64_t epoch = 0;
    std::size_t next = 0, received = 0;
};

void replay(const Timeline &timeline, unsigned lane, Frame length, std::uint32_t rate,
            const char *model, const char *root, std::ostream &output) {
    const auto attack = (5 * rate + 999) / 1000;
    const auto &voices = timeline.voices;
    std::array<float, block> now{}, prior{};
    auto current = std::make_unique<Voice>(voices.front(), lane, rate, model, root);
    std::unique_ptr<Voice> old;
    std::size_t generation = 0, gainIndex = 0;
    double gain = 1.;
    std::uint64_t hash = offsetBasis, window = offsetBasis;
    for (Frame frame = 0; frame < length;) {
        if (old && frame == voices[generation - 1].finish) {
            old->finish();
            old.reset();
        }
        if (generation + 1 < voices.size() && frame == voices[generation + 1].start) {
            old = std::move(current);
            current = std::make_unique<Voice>(voices[++generation], lane, rate, model, root);
        }
        const auto &voice = voices[generation];
        if (gainIndex < timeline.gains.size() && frame == timeline.gains[gainIndex].frame) {
            do { gain = timeline.gains[gainIndex++].value; }
            while (gainIndex < timeline.gains.size() && frame == timeline.gains[gainIndex].frame);
        }
        Frame end = std::min(frame + block, length);
        if (generation + 1 < voices.size()) end = std::min(end, voices[generation + 1].start);
        if (old) end = std::min(end, voices[generation - 1].finish);
        if (gainIndex < timeline.gains.size()) end = std::min(end, timeline.gains[gainIndex].frame);
        const auto count = static_cast<unsigned>(end - frame);
        current->render(count, now.data());
        if (old) old->render(count, prior.data());
        for (unsigned i = 0; i < count; ++i) {
            const auto f = frame + i;
            double sample = now[i];
            if (old) {
                const auto elapsed = f - voice.start;
                const auto weight = static_cast<double>(elapsed) / voice.overlap;
                sample = elapsed == 0 ? prior[i] : (1. - weight) * prior[i] + weight * sample;
            }
            const auto gate = f < attack ? static_cast<double>(f) / attack : 1.;
            const float value = gain == 0 || gate == 0 ? 0.f : static_cast<float>(sample * gain * gate);
            std::uint32_t bits;
            std::memcpy(&bits, &value, sizeof(value));
            hash = (hash ^ bits) * prime;
            window = (window ^ bits) * prime;
            if ((f + 1) % rate == 0 || f + 1 == length) {
                output << lane << '\t' << f / rate << '\t' << window << '\n';
                window = offsetBasis;
            }
        }
        frame = end;
    }
    if (old) old->finish();
    current->finish();
    output << lane << "\tall\t" << hash << '\n';
}
}

int main(int argc, char **argv) {
    if (argc != 9) return 2;
    try {
        const auto rate = integer<std::uint32_t>(argv[7]);
        const auto frames = integer<Frame>(argv[8]);
        require((rate == 44100 || rate == 48000) && frames && frames <= rate * UINT64_C(600), "invalid replay length or rate");
        const auto lanes = read(argv[1], frames, rate);
        std::ofstream output(argv[2]);
        require(static_cast<bool>(output), "open replay digest output");
        output << "lane\twindow\tdigest\n";
        replay(lanes[0], 0, frames, rate, argv[3], argv[4], output);
        replay(lanes[1], 1, frames, rate, argv[5], argv[6], output);
        require(static_cast<bool>(output), "write replay digests");
        return 0;
    } catch (const std::exception &issue) {
        std::cerr << issue.what() << '\n';
        return 1;
    }
}
