# Experimental Fleet

The build-tree C11 and C++17 interface in `engine_sim/fleet.h` is available through
`EngineSim::Fleet` when the device-free runtime is enabled. This increment supports
owner creation, detached voice preparation, ordered commands, one serial
producer and a planar callback.

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
quiescence. For optional diagnostics, initialize `es_rt_diagnostic.size` and
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
The experimental worker supports one occupied lane, complete physical
vectors and zero-duration activation or stop. Start rejects a pending command
outside this subset. While running, unsupported commands are rejected at submit.

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

### Physical control policy

Choose `ES_RT_GAME` when preparing a voice for time-shaped controls. The direct
throttle reaches requested demand over 10 ms of numerical ticks. A governor
receives the new setpoint at the first eligible tick. The requested load in
N·m ramps over 5 ms. Both ramps round up to a whole tick. In DRIVEN mode the
shaft target slews up or down by at most 2,400 RPM per second, within the
engine's validated range. A request for zero RPM decelerates toward a stationary
shaft. Ignition and starter take effect at the first eligible tick. The starter
is available in FREE mode.

A retarget begins from the trajectory's value at that tick. Sending the same
target preserves the current trajectory. Choose `ES_RT_RAW` for step changes
at the first eligible tick and for comparison with the v2 raw stem. The shaft
solver retains its existing authority checks in both policies. A valid target
can still lead to a numerical fault.

Snapshot `requested` is the full target associated with the applied event.
Snapshot `applied` contains the values passed to the engine at the most recent
numerical tick start. Measured RPM, plate angle and reactions come from the
last completed engine frame.

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
tile payload and tags. Host and device qualification remain pending.

## Starting the producer

Prepare the voice, then submit `ES_RT_ACTIVATE` at frame zero with the prepared
token, lane generation 1 and `duration_ms = 0`. Schedule any
initial vectors before `es_rt_start`. Start launches one library-owned producer
and returns after retaining exactly `H` frames. Start the host callback after
that return. The first callback starts at frame zero. The RAW stem has unity
gain and uses the same numerical pipeline as v2. The reference mix applies
the default master gain of 0.5.

While running, `es_rt_get_transport` reports separate observations of submitted
frames `D`, published frames `G` and the first unsealed frame `S`. Schedule
from a fresh observation. `D + H + 64` is the practical earliest request for
additional sequencing margin on this transport. The producer refills when
the number of frames ahead reaches the low watermark. Its target never exceeds
`D + H`, and it generates only whole 64-frame tiles. A failed voice preserves
its completed prefix and contributes zero for the rest of the tile and
later spans.

Stop and drain the host callback before calling `es_rt_quiesce`. Quiesce joins
the producer even if no further frames will be consumed. Create a new fleet
with a fresh epoch to restart. Lifecycle envelopes are pending.

## Observing voices and reclaiming cells

`es_rt_poll_snapshot` takes the oldest available snapshot of a completed
quantum. It includes the generation, requested and physically latched control
vectors, tick counts and measured state. Its `generated` field identifies the
audio position covered by that snapshot. The audio driver's presentation
position is a separate observation.

The producer retains up to 16 snapshots. When the queue fills it drops new
snapshots while PCM and commands continue. Each new snapshot carries the
cumulative number dropped so far. `es_rt_get_transport` reports the current
drop count. Drain the queue when observation age matters. Command results
and returned cells remain available while snapshots are being dropped.

Poll command results for terminal status and completion frames. Use
`es_rt_collect` to take a returned cell and obtain its historical token and
final voice observation. Collection destroys the engine on the coordinator
thread, freeing its cell for a future voice. The returned token identifies
the old engine. A rejected or detached voice remains in its cell until
collected. Use `es_rt_discard` for a token that you prepared and never submitted.

For shutdown, stop host callbacks and wait for them to return, then call
`es_rt_quiesce` on the coordinator. The producer joins and pending commands
receive terminal cancellation receipts. The settled difference between `G`
and `D`, when positive, counts generated frames abandoned at shutdown. Drain
snapshots, receipts and returned cells as needed, discard any untransferred
tokens, and destroy the fleet after all users are done.
