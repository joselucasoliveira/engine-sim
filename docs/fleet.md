# Experimental Fleet preparation

The build-tree C11 and C++17 interface in `engine_sim/fleet.h` is available through
`EngineSim::Fleet` when the device-free runtime is enabled. This increment supports
owner creation, detached voice preparation, discard and destruction.

Initialize each extensible input with its size and `ES_RT_V1`. For local
experiments, create an owner with `ES_RT_EXPERIMENTAL` and qualification ID zero.
Use a distinct nonzero epoch for each owner and restart. Valid rates are 44100
and 48000 Hz. The Balanced profile accepts maximum blocks of 64, 128, 256, 512
or 1024 frames and defaults to 256. Interactive accepts 64 or 128 frames.
Choose one to eight logical lanes. Each owner reserves three additional
physical cell slots for candidate preparation and later replacement.

Use `es_rt_prepare_file` with absolute UTF-8 entry and resource paths or
`es_rt_prepare_bytes` with an authored JSON span and an absolute UTF-8 resource
root. Preparation copies the document, pins source bytes and validates the
selected model and initial physical control vector. The two candidate credits
limit outstanding prepared voices. On failure the token destination and
existing candidates retain their values. Release an unused token with
`es_rt_discard`. Tokens carry the owning epoch and a cell generation.

The non-audio control owner serializes these calls and destroys the fleet after
its last use. For optional diagnostics, initialize `es_rt_diagnostic.size` and
`es_rt_diagnostic.abi`. The call writes a bounded UTF-8 message and numeric stage
and status. Success clears the diagnostic.

## Command admission

One control owner serializes submissions and result polling. Submit complete
events with a strictly increasing sequence and nondecreasing output frame.
The fleet copies each accepted event and reserves a result ticket for it.
`ES_RT_OK` from `es_rt_submit` means the command is queued. Poll its sequence
with `es_rt_poll_result` to obtain the terminal disposition. Polling reports
`ES_RT_PENDING` while the command is outstanding and consumes a terminal result
on success. Poll a terminal result once to return its ticket credit.

The current transport supports 256 ingress entries and 256 tickets. Structural
events retain eight reserved entries and tickets. A full or otherwise rejected
submission preserves the event sequence, candidate token and lane reservation.
Worker integration and audio generation belong to later increments.

## Output frames and numerical ticks

The scheduler orders events by absolute output frame and sequence. It seals one
64-frame tile at a time. An event that arrives after the seal snapshot can
receive a terminal `ES_RT_LATE` even when submission accepted it. The control
side also rejects requests more than two seconds ahead of reported submission.

Physical controls take effect at the first eligible numerical tick start. A
tick that completes on a frame boundary belongs to the preceding interval.
For example, two vectors queued at frame 64 are both applied in sequence at
frame 64. The next tick observes the second vector if both arrive before it
starts. The scheduler preserves the receipt for each applied vector.

To schedule from a host clock, establish a monotonic-time and estimated DAC
frame pair outside the engine. Convert later times to output frames by rounding
up, using the fleet sample rate. Establish a fresh mapping after an epoch break
or a device clock discontinuity.

## Planar audio callback

Only one audio callback may use an owner at a time. Supply separate, naturally
aligned float arrays for any combination of the mono mix and configured stems.
Use null pointers for skipped planes and lanes outside the configured count.
Every supplied array must hold at least `capacity` samples and stay valid for
the call. Pre-touch the arrays off the audio thread. For example, with two
configured lanes and a requested maximum of 256 frames:

```c
float mix[256] = {0}, rear[256] = {0};
es_rt_audio audio = {sizeof(audio), ES_RT_V1, 128, 256, mix, {0, rear}};
es_rt_block result = {sizeof(result), ES_RT_V1, 0, 0, 0, 0};
int32_t status = es_rt_process(fleet, &audio, &result);
```

The function accepts requests of zero to the configured maximum, with at least
one selected destination for positive frame counts. Zero frames report the
current position and leave the transport untouched. A rejected request leaves
the destinations, timeline and result unchanged. `ES_RT_BROKEN` reports a valid
request with a verified prefix in `result.pcm_frames` and positive-zero samples
in each selected plane for the remaining `result.fallback_frames`. Further
valid requests in that epoch advance the submission position and return
silence. Quiesce the callback before releasing its owner.

Balanced uses `H = 4 * max_frames` and Interactive uses `H = 3 * max_frames`.
Creation reserves `H / 64 + 2` tagged tiles, each with one 64-frame mix and
eight 64-frame stems. At Balanced 256 this is 18 tiles, with 41,760 bytes of
tile payload and tags. The current producer is a test fixture that writes
tagged shape samples. Engine priming and host/device qualification are pending.
