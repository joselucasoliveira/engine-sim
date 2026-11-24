# Fleet

The build-tree C11 and C++17 interface in `engine_sim/fleet.h` is available through
`EngineSim::Fleet` when the device-free runtime is enabled. This increment supports
owner creation, detached voice preparation, ordered commands, one serial
producer and a planar callback.

Initialize each extensible input with its size and `ES_RT_V1`. Create an owner
with flags and qualification ID set to zero. Existing clients may continue to
pass the legacy `ES_RT_EXPERIMENTAL` flag.
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
The worker supports the configured number of independent lanes
and one replacement at a time.

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
its completed prefix and applies a 32-frame decay from its last valid source
sample.

Stop and drain the host callback before calling `es_rt_quiesce`. Quiesce joins
the producer even if no further frames will be consumed. Create a new fleet
with a fresh epoch to restart.

## Gain and lifecycle

Gain and master are linear amplitude in [0,1]. They default to 1 and 0.5
respectively. Activation, gain, mute, unmute, pause, resume and stop use a
5 ms ramp by default. Set `duration_ms` to `ES_RT_DFLT_MS` for the default
or an integer from 0 to 100. Zero takes effect at the requested output
frame. A positive duration rounds up to a whole number of output frames.
An interrupted transition starts from its value at the interruption frame.
Restating the same target leaves the original endpoint in place.

Mute sets the lane gate to zero while physics, filters and clocks continue.
Pause fades to zero and then freezes those histories. The lane remembers
its mute setting while paused. Resume continues from the retained history
and restores the remembered gate target. Ignition changes combustion at a
numerical tick, while mechanical sound and previously generated filter
history may continue. Stop retires the engine at its zero endpoint and
truncates its filter tail there. A silent gate permits immediate retirement.

Lifecycle tickets complete when their transition reaches its endpoint.
On interruption, the old ticket reports `ES_RT_INTERRUPTED` and the
interruption frame. Shutdown preserves an applied frame on a canceled
transition if the ramp had already begun.

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


## Independent lanes and the reference mix

Each occupied lane owns an independent model, seed, clock, solver and DSP
history. The producer advances occupied lanes in ascending index. An unused
or fully paused lane supplies positive zero. Adding another voice leaves the
existing stems and their gains unchanged.

Each output stem is mono binary32 with the lane's gain and gate applied.
The reference bus adds the stems as binary64 values in ascending lane order,
applies master gain, clamps to [-1,1] and casts to binary32. The mix and stems
requested in the same callback describe the same frames. Later calls consume
later frames. Use activation receipts and their effective frame and generation
to associate older samples with an engine. Hosts can spatialize stems or
duplicate the reference mix to device channels.

The limit is eight logical lanes with `V + 3` physical cells
and two preparation credits. A returned cell retains its accounted payload
until collected. The fixed-storage ceiling is 8 MiB and each live cell is
bounded to 16 MiB of accounted payload. Thread stacks, allocation overhead,
transient preparation and device buffers are measured separately. Real-time
performance depends on the host, audio backend and active engines.


## Replacement and numerical faults

Prepare a separate token, then submit `ES_RT_REPLACE` with the current lane
identity and its next generation. One overlap permit remains held until the
replacement receipt is consumed. The default duration is 20 ms. The new
engine starts at local frame zero and the old one keeps its preceding target.
The lane blends the sources using complementary weights and keeps its gain
and mute setting. At the exact endpoint only the new source renders. A
zero-duration replacement detaches the old engine at the requested frame.

While an overlap is active, new physical targets use the new identity. The
old identity no longer accepts controls. Other lanes continue independently.
The old cell remains reserved until the endpoint and then awaits collection.

A numerical fault preserves the completed prefix and the last successful
mechanical state. The source contributes its last valid value with weights
31/32, 30/32 through 0/32 on successive frames. A source with no completed
samples contributes zero. Its physics and DSP stop at the fault. The decay
uses the existing gain and gate. A later stop can fade from the residual
value and retire early if it is already silent.

A fault in either side of a replacement leaves the original crossfade and
endpoint in place. The replacement ticket reports `ES_RT_FAULT` with its
applied and end frames. The new generation remains the lane's identity.
Inspect the returned old generation and the current snapshot for each
source's fault and completed counters.

## Recovering from a transport break

For a valid broken span, the first fallback frame is `block.first +
block.pcm_frames`. The returned PCM count and fallback count add up to the
requested size. Every selected plane is initialized. Subsequent valid spans
continue advancing submitted `D` and return silence until shutdown. The
producer's published `G` and the engines' completed counters remain separate.

If the device disappears, changes rate or reports a discontinuity, stop
and drain its callbacks using the host's off-audio control path. Record the
settled `D`, `G`, break status and any host-reported presentation gap.
Quiesce the fleet even if the device never started. Recover terminal receipts
and returned cells, then destroy that owner. Create a fresh epoch, prepare
the new voices, schedule initial controls and prime with `es_rt_start` before
opening the new stream. A new epoch starts at frame zero with new tokens and
clock calibration.

A raw v2 numerical fault returns its completed prefix. The Fleet producer
can continue publishing a complete bus around an isolated numerical fault.
A broken Fleet transport reports its verified prefix and fills the remainder
of the callback with silence. Actual device recovery timing needs the host's
clock and endpoint evidence.

The [two-vehicle examples](../examples/fleet/README.md) build C11 and C++17
hosts against the public Fleet interface and use synthetic licensed assets.
They model control, audio and shutdown roles without a device backend.

## Installed FleetSDK

Use `find_package(EngineSimFleet 1.0 CONFIG REQUIRED)` and link
`EngineSim::Fleet` from a standalone C11 or C++17 project. Static consumers
need a C++ final linker. The imported target carries the required static
definition, private archives and thread dependency. The installed shared
variant exports the Fleet C entry points. SDK locations are relative to the
installation, so the entire prefix can move before configuring the consumer.

Fleet and the v2 `EngineSimRuntime` package can live in one prefix and process.
Each API owns the handles it creates. Install `FleetSDKExamples` separately
for the model fixtures and sample hosts. See [build commands](../BUILDING.md).

The standalone example can optionally use a system PortAudio v19 provider.
It reports callback and DAC-estimate timestamps separately from Fleet's
submitted frame count. See [live host setup](../examples/fleet/README.md).
