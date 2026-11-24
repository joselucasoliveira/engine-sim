/* The supervisor paces this C11 example. The audio entry calls es_rt_process. */
#if !defined(_WIN32)
# define _POSIX_C_SOURCE 200809L
#endif
#include "engine_sim/fleet.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
# include <windows.h>
static void poll_delay(void) { Sleep(1); }
#else
# include <time.h>
static void poll_delay(void) {
    const struct timespec one_ms = {0, 1000000L};
    (void)nanosleep(&one_ms, NULL);
}
#endif

enum { block_frames = 256, rate = 48000, voices = 2, commands = 7 };
static const uint64_t epoch = 1;
static const uint64_t stop_at[voices] = {60000, 66000};

struct host {
    es_rt_fleet *fleet;
    float mix[block_frames + 2], stem[voices][block_frames + 2];
    es_rt_audio span;
    es_rt_block report;
};

static es_rt_id lane_id(unsigned lane) {
    es_rt_id id = {0};
    id.generation = 1;
    id.lane = lane;
    return id;
}

static es_rt_event make_event(unsigned kind, uint64_t seq, uint64_t frame) {
    es_rt_event e = {0};
    e.size = sizeof e;
    e.abi = ES_RT_V1;
    e.kind = kind;
    e.sequence = seq;
    e.epoch = epoch;
    e.frame = frame;
    return e;
}

static int submit_events(struct host *h, es_rt_token candidate[voices]) {
    for (unsigned i = 0; i < voices; ++i) {
        es_rt_event activate = make_event(ES_RT_ACTIVATE, 1 + i, 0);
        activate.duration_ms = ES_RT_DFLT_MS;
        activate.next_voice = lane_id(i);
        activate.candidate = candidate[i];
        if (es_rt_submit(h->fleet, &activate) != ES_RT_OK) return 0;
    }
    return 1;
}

/* C owns this call. No new deadlines need be chosen by the audio thread. */
static int schedule_drive_and_stop(struct host *h) {
    es_rt_event spin = make_event(ES_RT_VECTOR, 3, 4800);
    spin.voice = lane_id(1);
    spin.physical.ignition = 1;
    spin.physical.demand = 0.3;
    spin.physical.driven_rpm = 600.0;
    if (es_rt_submit(h->fleet, &spin) != ES_RT_OK) return 0;

    es_rt_event release_starter = make_event(ES_RT_VECTOR, 4, 16000);
    release_starter.voice = lane_id(0);
    release_starter.physical.ignition = 1;
    release_starter.physical.demand = 0.55;
    if (es_rt_submit(h->fleet, &release_starter) != ES_RT_OK) return 0;

    es_rt_event loaded = make_event(ES_RT_VECTOR, 5, 25200);
    loaded.voice = lane_id(1);
    loaded.physical.ignition = 1;
    loaded.physical.demand = 0.65;
    loaded.physical.driven_rpm = 900.0;
    loaded.physical.load_nm = 2.0;
    if (es_rt_submit(h->fleet, &loaded) != ES_RT_OK) return 0;
    for (unsigned i = 0; i < voices; ++i) {
        es_rt_event stop = make_event(ES_RT_STOP, 6 + i, stop_at[i]);
        stop.voice = lane_id(i);
        stop.duration_ms = 5;
        if (es_rt_submit(h->fleet, &stop) != ES_RT_OK) return 0;
    }
    return 1;
}

/* Actual callback boundary. Storage and record headers initialized by the host. */
static int32_t audio_callback(struct host *h, uint32_t frames) {
    h->span.frames = frames;
    return es_rt_process(h->fleet, &h->span, &h->report);
}

/* Test-only input driver on the supervisor. A device must call at its own pace. */
static int wait_for_pcm(es_rt_fleet *fleet, uint64_t end) {
    for (unsigned attempt = 0; attempt < 3000; ++attempt) {
        es_rt_transport t = {0};
        t.size = sizeof t;
        t.abi = ES_RT_V1;
        if (es_rt_get_transport(fleet, &t) != ES_RT_OK || t.broken) return 0;
        if (t.generated >= end) return 1;
        poll_delay();
    }
    return 0;
}

struct observations {
    uint64_t ticks[voices], nonzero[voices];
    unsigned targets;
};
static int observe_voices(es_rt_fleet *fleet, struct observations *witness) {
    for (unsigned i = 0; i < 16; ++i) {
        es_rt_snapshot s = {0};
        s.size = sizeof s;
        s.abi = ES_RT_V1;
        const int32_t status = es_rt_poll_snapshot(fleet, &s);
        if (status == ES_RT_EMPTY) break;
        if (status != ES_RT_OK || s.epoch != epoch || s.lanes != voices || s.dropped) return 0;
        for (unsigned lane = 0; lane < voices; ++lane) {
            const es_rt_lane_info *v = &s.lane[lane];
            if (v->fault || (v->voice.generation && (v->voice.generation != 1 || v->voice.lane != lane))) return 0;
            if (!v->voice.generation) continue;
            if (v->ticks < witness->ticks[lane]) return 0;
            witness->ticks[lane] = v->ticks;
            if (lane == 0 && v->requested_frame == 16000 && v->applied.starter == 0) witness->targets |= 1;
            if (lane == 1 && v->requested.driven_rpm == 600 && v->applied.driven_rpm == 600) witness->targets |= 2;
            if (lane == 1 && v->requested.driven_rpm == 900 && v->applied.driven_rpm == 900 &&
                    v->applied.load_nm == 2) witness->targets |= 4;
        }
    }
    return 1;
}

static int initialize_audio(struct host *h) {
    h->span.size = sizeof h->span;
    h->span.abi = ES_RT_V1;
    h->span.capacity = block_frames;
    h->span.mix = h->mix + 1;
    for (unsigned i = 0; i < voices; ++i) h->span.stem[i] = h->stem[i] + 1;
    h->report.size = sizeof h->report;
    h->report.abi = ES_RT_V1;
    h->report.first = 42;
    h->mix[1] = 42;
    return audio_callback(h, 1) == ES_RT_STATE && h->report.first == 42 && h->mix[1] == 42;
}

static int consume(struct host *h) {
    const uint64_t end = 68000;
    struct observations witness = {0};
    uint64_t first = 0;
    uint32_t blocks = 0, last = 0;
    while (first < end) {
        const uint32_t n = end - first < block_frames ? (uint32_t)(end - first) : block_frames;
        if (!wait_for_pcm(h->fleet, first + n)) return 0;
        h->mix[0] = h->mix[n + 1] = 81;
        for (unsigned i = 0; i < voices; ++i) h->stem[i][0] = h->stem[i][n + 1] = 82;
        if (audio_callback(h, n) != ES_RT_OK) return 0;
        if (h->report.first != first || h->report.epoch != epoch ||
            h->report.pcm_frames != n || h->report.fallback_frames != 0) return 0;
        if (h->mix[0] != 81 || h->mix[n + 1] != 81) return 0;
        for (unsigned i = 0; i < voices; ++i)
            if (h->stem[i][0] != 82 || h->stem[i][n + 1] != 82) return 0;
        for (uint32_t f = 1; f <= n; ++f) {
            const double expected = 0.5 * ((double)h->stem[0][f] + (double)h->stem[1][f]);
            const float mixed = expected < -1.0 ? -1.0f : expected > 1.0 ? 1.0f : (float)expected;
            if (!isfinite(h->mix[f]) || h->mix[f] != mixed) return 0;
            for (unsigned i = 0; i < voices; ++i) {
                if (!isfinite(h->stem[i][f])) return 0;
                if (h->stem[i][f] != 0.0f) ++witness.nonzero[i];
            }
        }
        if (!observe_voices(h->fleet, &witness)) return 0;
        first += n;
        ++blocks;
        last = n;
    }
    es_rt_transport transport = {0};
    transport.size = sizeof transport;
    transport.abi = ES_RT_V1;
    if (es_rt_get_transport(h->fleet, &transport) != ES_RT_OK || transport.submitted != end ||
            transport.broken || transport.generated < end || transport.generated - end > transport.high_frames ||
            witness.targets != 7 || last != 160) return 0;
    for (unsigned i = 0; i < voices; ++i) {
        if (!witness.nonzero[i] || !witness.ticks[i]) return 0;
        printf("lane=%u ticks=%" PRIu64 " nonzero=%" PRIu64 "\n", i, witness.ticks[i], witness.nonzero[i]);
    }
    printf("submitted=%" PRIu64 " generated=%" PRIu64 " blocks=%" PRIu32 " last=%" PRIu32 "\n",
           transport.submitted, transport.generated, blocks, last);
    return 1;
}

static int check_receipts(es_rt_fleet *fleet) {
    for (uint64_t seq = 1; seq <= commands; ++seq) {
        es_rt_result r = {0};
        r.size = sizeof r;
        r.abi = ES_RT_V1;
        if (es_rt_poll_result(fleet, seq, &r) != ES_RT_OK ||
            r.sequence != seq || r.epoch != epoch || r.reason != ES_RT_OK) return 0;
        if (seq <= 2 && (r.kind != ES_RT_ACTIVATE || r.applied != 0 || r.ended != 240)) return 0;
        if (seq >= 3 && seq <= 5) {
            const uint64_t frames[] = {4800, 16000, 25200};
            if (r.kind != ES_RT_VECTOR || r.applied != frames[seq - 3] || r.ended != r.applied) return 0;
        }
        if (seq >= 6) {
            const uint64_t stop = stop_at[seq - 6];
            if (r.kind != ES_RT_STOP || r.applied != stop || r.ended != stop + 240) return 0;
            printf("stop lane=%" PRIu64 " start=%" PRIu64 " end=%" PRIu64 "\n", seq - 6, r.applied, r.ended);
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 5 || strcmp(argv[1], argv[3]) == 0) {
        fprintf(stderr, "usage: host ABS_MODEL_A ABS_ROOT_A ABS_DISTINCT_MODEL_B ABS_ROOT_B\n");
        return EXIT_FAILURE;
    }
    if (es_rt_api_version() != ES_RT_V1) return EXIT_FAILURE;
    struct host h = {0};
    es_rt_config cfg = {0};
    cfg.size = sizeof cfg;
    cfg.abi = ES_RT_V1;
    cfg.epoch = epoch;
    cfg.rate = rate;
    cfg.profile = ES_RT_BALANCED;
    cfg.max_frames = block_frames;
    cfg.lanes = voices;
    /* Functional example independent of installed native qualification rows. */
    cfg.flags = ES_RT_EXPERIMENTAL;
    es_rt_diagnostic d = {0};
    d.size = sizeof d;
    d.abi = ES_RT_V1;
    if (es_rt_create(&cfg, &h.fleet, &d) != ES_RT_OK) {
        fprintf(stderr, "fleet creation failed: %s\n", d.message);
        return EXIT_FAILURE;
    }

    es_rt_token candidate[voices] = {0};
    int passed = 0;
    if (!initialize_audio(&h)) goto finish;
    for (unsigned i = 0; i < voices; ++i) {
        es_rt_voice_config v = {0};
        v.size = sizeof v;
        v.abi = ES_RT_V1;
        v.mode = i == 0 ? ES_RT_FREE : ES_RT_DRIVEN;
        v.controls_policy = ES_RT_GAME;
        v.seed = 71 + i;
        v.initial.demand = 0.25;
        v.initial.ignition = 1;
        v.initial.starter = i == 0 ? 1u : 0u;
        if (es_rt_prepare_file(h.fleet, &v, argv[1 + 2*i], argv[2 + 2*i], &candidate[i], &d) != ES_RT_OK) {
            fprintf(stderr, "prepare %u failed: %s\n", i, d.message);
            goto finish;
        }
    }
    if (!submit_events(&h, candidate)) goto finish;
    if (es_rt_start(h.fleet, &d) != ES_RT_OK) goto finish;
    if (!schedule_drive_and_stop(&h) || !consume(&h) || !check_receipts(h.fleet)) goto finish;

    {
        unsigned returned = 0;
        for (unsigned i = 0; i < voices; ++i) {
            es_rt_token token;
            es_rt_lane_info final;
            if (es_rt_collect(h.fleet, &token, &final) != ES_RT_OK) goto finish;
            const unsigned lane = final.voice.lane;
            if (lane >= voices || final.voice.generation != 1 || (returned & (1u << lane)) ||
                final.fault != 0 || final.frames != stop_at[lane] + 240 ||
                final.ticks != final.frames * 10000 / rate || token.epoch != epoch ||
                token.cell != candidate[lane].cell || token.generation != candidate[lane].generation) goto finish;
            returned |= 1u << lane;
            printf("returned lane=%u frames=%" PRIu64 " ticks=%" PRIu64 "\n", lane, final.frames, final.ticks);
        }
        es_rt_token empty_token;
        es_rt_lane_info empty_voice;
        passed = returned == (1u << voices) - 1 &&
                 es_rt_collect(h.fleet, &empty_token, &empty_voice) == ES_RT_EMPTY;
    }
finish:
    /* Single-threaded fake backend has no callbacks in flight here. A real host
     * first stops/drains the backend. Quiesce also covers partial setup/start. */
    if (es_rt_quiesce(h.fleet) != ES_RT_OK) return EXIT_FAILURE;
    for (unsigned i = 0; i < voices; ++i) {
        if (candidate[i].epoch) (void)es_rt_discard(h.fleet, candidate[i]);
    }
    {
        es_rt_token token;
        es_rt_lane_info state;
        while (es_rt_collect(h.fleet, &token, &state) == ES_RT_OK) {}
    }
    es_rt_destroy(h.fleet);
    if (!passed) fputs("fleet host scenario failed\n", stderr);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
