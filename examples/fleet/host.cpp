#include "engine_sim/fleet.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
constexpr std::uint32_t rate = 48000, maximum = 256;
constexpr std::uint64_t epoch = 1, finalFrame = 68000;
constexpr std::array<std::uint64_t, 2> stopAt{60000, 66000};

void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Record>
Record record() {
    Record value{};
    value.size = sizeof(Record);
    value.abi = ES_RT_V1;
    return value;
}

es_rt_event command(std::uint32_t kind, std::uint64_t sequence, std::uint64_t frame) {
    auto event = record<es_rt_event>();
    event.epoch = epoch;
    event.kind = kind;
    event.sequence = sequence;
    event.frame = frame;
    return event;
}

struct Audio {
    es_rt_fleet *fleet = nullptr;
    std::array<std::array<float, maximum + 2>, 3> planes{};
    es_rt_audio span = record<es_rt_audio>();
    es_rt_block result = record<es_rt_block>();

    void bind(es_rt_fleet *owner) {
        fleet = owner;
        span.capacity = maximum;
        span.mix = planes[0].data() + 1;
        span.stem[0] = planes[1].data() + 1;
        span.stem[1] = planes[2].data() + 1;
    }

    // The backend calls this entry using already prepared arrays.
    std::int32_t callback(std::uint32_t frames) noexcept {
        span.frames = frames;
        return es_rt_process(fleet, &span, &result);
    }
};

class Host {
public:
    Host() {
        auto config = record<es_rt_config>();
        config.rate = rate;
        config.profile = ES_RT_BALANCED;
        config.max_frames = maximum;
        config.lanes = 2;
        config.epoch = epoch;
        auto diagnostic = record<es_rt_diagnostic>();
        es_rt_fleet *fleet = nullptr;
        check(es_rt_create(&config, &fleet, &diagnostic) == ES_RT_OK, diagnostic.message);
        owner.reset(fleet);
        audio.bind(fleet);
    }
    ~Host() {
        if (es_rt_quiesce(owner.get()) != ES_RT_OK) return;
        for (const auto token : candidates) {
            if (token.epoch) (void)es_rt_discard(owner.get(), token);
        }
        es_rt_token token{};
        es_rt_lane_info state{};
        while (es_rt_collect(owner.get(), &token, &state) == ES_RT_OK) {}
    }

    void run(const std::array<const char *, 4> &paths) {
        audio.result.first = 42;
        audio.planes[0][1] = 42.f;
        check(audio.callback(1) == ES_RT_STATE && audio.result.first == 42 && audio.planes[0][1] == 42.f,
              "callback before priming");
        auto diagnostic = record<es_rt_diagnostic>();
        for (std::uint32_t lane = 0; lane < candidates.size(); ++lane) {
            auto config = record<es_rt_voice_config>();
            config.mode = lane == 0 ? ES_RT_FREE : ES_RT_DRIVEN;
            config.controls_policy = ES_RT_GAME;
            config.seed = 71 + lane;
            config.initial = {.25, 0., 0., 1, lane == 0 ? 1u : 0u};
            check(es_rt_prepare_file(owner.get(), &config, paths[2 * lane], paths[2 * lane + 1],
                                      &candidates[lane], &diagnostic) == ES_RT_OK, diagnostic.message);
        }
        for (std::uint32_t lane = 0; lane < candidates.size(); ++lane) {
            auto activate = command(ES_RT_ACTIVATE, 1 + lane, 0);
            activate.duration_ms = ES_RT_DFLT_MS;
            activate.candidate = candidates[lane];
            activate.next_voice = {1, lane, 0};
            submit(activate);
        }
        check(es_rt_start(owner.get(), &diagnostic) == ES_RT_OK, diagnostic.message);
        check(position().sealed <= 4800u, "unexpected startup control horizon");
        request(3, 4800, 1, {.3, 0., 600., 1, 0});
        request(4, 16000, 0, {.55, 0., 0., 1, 0});
        request(5, 25200, 1, {.65, 2., 900., 1, 0});
        for (std::uint32_t lane = 0; lane < stopAt.size(); ++lane) {
            auto stop = command(ES_RT_STOP, 6 + lane, stopAt[lane]);
            stop.duration_ms = 5;
            stop.voice = {1, lane, 0};
            submit(stop);
        }
        consume();
        check(es_rt_quiesce(owner.get()) == ES_RT_OK, "quiesce simulated backend");
        checkReceipts();
        collect();
    }

private:
    void submit(const es_rt_event &event) const {
        check(es_rt_submit(owner.get(), &event) == ES_RT_OK, "fleet command admission");
    }
    void request(std::uint64_t sequence, std::uint64_t frame, std::uint32_t lane, es_rt_vector target) const {
        auto event = command(ES_RT_VECTOR, sequence, frame);
        event.voice = {1, lane, 0};
        event.physical = target;
        submit(event);
    }
    es_rt_transport position() const {
        auto status = record<es_rt_transport>();
        check(es_rt_get_transport(owner.get(), &status) == ES_RT_OK && !status.broken, "transport status");
        return status;
    }
    void waitForPcm(std::uint64_t end) const {
        using namespace std::chrono_literals;
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (position().generated < end) {
            check(std::chrono::steady_clock::now() < deadline, "simulated driver waiting for PCM");
            std::this_thread::sleep_for(100us);
        }
    }
    void observe() {
        auto snapshot = record<es_rt_snapshot>();
        for (unsigned interested = 0; interested < 16; ++interested) {
            const auto status = es_rt_poll_snapshot(owner.get(), &snapshot);
            if (status == ES_RT_EMPTY) return;
            check(status == ES_RT_OK && snapshot.epoch == epoch && snapshot.lanes == 2 &&
                  !snapshot.dropped, "voice snapshot");
            for (std::uint32_t lane = 0; lane < ticks.size(); ++lane) {
                const auto &value = snapshot.lane[lane];
                check(!value.fault, "voice fault");
                if (!value.voice.generation) continue;
                check(value.voice.lane == lane && value.voice.generation == 1 && value.ticks >= ticks[lane],
                      "voice identity or completed ticks");
                ticks[lane] = value.ticks;
                if (lane == 0 && value.requested_frame == 16000 && value.applied.starter == 0) targets |= 1;
                if (lane == 1 && value.requested.driven_rpm == 600 && value.applied.driven_rpm == 600) targets |= 2;
                if (lane == 1 && value.requested.driven_rpm == 900 && value.applied.driven_rpm == 900 &&
                        value.applied.load_nm == 2) targets |= 4;
            }
        }
    }
    void consume() {
        std::uint32_t blocks = 0, last = 0;
        for (std::uint64_t first = 0; first < finalFrame;) {
            const auto frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(maximum, finalFrame - first));
            waitForPcm(first + frames);
            for (auto &plane : audio.planes) plane.front() = plane[frames + 1] = 47.f;
            check(audio.callback(frames) == ES_RT_OK && audio.result.epoch == epoch && audio.result.first == first &&
                  audio.result.pcm_frames == frames && !audio.result.fallback_frames, "complete callback");
            for (const auto &plane : audio.planes) {
                check(plane.front() == 47.f && plane[frames + 1] == 47.f, "span bounds");
            }
            for (std::uint32_t i = 1; i <= frames; ++i) {
                const float a = audio.planes[1][i], b = audio.planes[2][i];
                const auto reference = static_cast<float>(std::clamp((static_cast<double>(a) + b) * .5, -1., 1.));
                check(std::isfinite(a) && std::isfinite(b) && audio.planes[0][i] == reference, "reference mix");
                if (a != 0.f) ++nonzero[0];
                if (b != 0.f) ++nonzero[1];
            }
            observe();
            first += frames;
            last = frames;
            ++blocks;
        }
        const auto final = position();
        check(final.submitted == finalFrame && final.generated >= finalFrame && final.generated - finalFrame <= final.high_frames
              && last == 160 && targets == 7, "final callback and control observation");
        for (unsigned lane = 0; lane < ticks.size(); ++lane) {
            check(ticks[lane] && nonzero[lane], "independent output");
            std::cout << "lane=" << lane << " ticks=" << ticks[lane] << " nonzero=" << nonzero[lane] << '\n';
        }
        std::cout << "submitted=" << final.submitted << " generated=" << final.generated
                  << " blocks=" << blocks << " last=" << last << '\n';
    }
    void checkReceipts() const {
        constexpr std::array<std::uint64_t, 7> starts{0,0,4800,16000,25200,stopAt[0],stopAt[1]};
        for (std::size_t i = 0; i < starts.size(); ++i) {
            auto result = record<es_rt_result>();
            const auto length = i < 2 || i >= 5 ? 240u : 0u;
            const std::uint32_t kind = i < 2 ? ES_RT_ACTIVATE : i < 5 ? ES_RT_VECTOR : ES_RT_STOP;
            check(es_rt_poll_result(owner.get(), i + 1, &result) == ES_RT_OK && result.epoch == epoch &&
                  result.reason == ES_RT_OK && result.kind == kind && result.applied == starts[i] &&
                  result.ended == starts[i] + length, "command receipt");
            if (i >= 5) std::cout << "stop lane=" << i - 5 << " start=" << result.applied << " end=" << result.ended << '\n';
        }
    }
    void collect() {
        std::array<bool, 2> returned{};
        es_rt_token token{};
        es_rt_lane_info final{};
        for (unsigned count = 0; count < returned.size(); ++count) {
            check(es_rt_collect(owner.get(), &token, &final) == ES_RT_OK, "returned engine");
            const auto lane = final.voice.lane;
            check(lane < returned.size() && !returned[lane] && final.voice.generation == 1 && !final.fault &&
                  token.epoch == epoch && token.cell == candidates[lane].cell &&
                  token.generation == candidates[lane].generation && final.frames == stopAt[lane] + 240 &&
                  final.ticks == final.frames * 10000 / rate, "retired voice identity and clock");
            returned[lane] = true;
            std::cout << "returned lane=" << lane << " frames=" << final.frames << " ticks=" << final.ticks << '\n';
        }
        check(es_rt_collect(owner.get(), &token, &final) == ES_RT_EMPTY, "one collection per generation");
    }

    std::unique_ptr<es_rt_fleet, decltype(&es_rt_destroy)> owner{nullptr, es_rt_destroy};
    std::array<es_rt_token, 2> candidates{};
    std::array<std::uint64_t, 2> ticks{}, nonzero{};
    unsigned targets = 0;
    Audio audio;
};
}

int main(int argc, char **argv) {
    if (argc != 5 || std::string_view(argv[1]) == argv[3]) {
        std::cerr << "usage: host ABS_MODEL_A ABS_ROOT_A ABS_DISTINCT_MODEL_B ABS_ROOT_B\n";
        return 1;
    }
    try {
        check(es_rt_api_version() == ES_RT_V1, "Fleet ABI version");
        Host host;
        host.run({argv[1], argv[2], argv[3], argv[4]});
        return 0;
    } catch (const std::exception &failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
