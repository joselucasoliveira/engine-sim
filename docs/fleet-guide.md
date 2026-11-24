# Integrate a vehicle fleet

Fleet 1.0 prepares independent vehicle engines, produces mono audio on its
worker and gives the host a mix and optional per-vehicle stems. Use
`<engine_sim/fleet.h>` and link `EngineSim::Fleet` from C11 or C++17. The
synchronous [Runtime 2.0 SDK](runtime-guide.md) can share the install prefix
and process. Keep each opaque handle with the API that created it.

## Install the SDK and examples

Use the runtime dependencies from [Building](../BUILDING.md). From the product
checkout, set `ES_OUT` to the prepared absolute build root, `FLEET_LINKAGE` to
`static` or `shared`, and `FLEET_PREFIX` to an absolute install prefix. Give
each linkage its own prefix. Use a native 64-bit toolchain and Ninja. On
Windows, run the commands from a VS 2022 x64 developer environment with the
Release DLL CRT and Git Bash.

```sh
cmake --preset "runtime-$FLEET_LINKAGE" -DENGINE_SIM_INSTALL_FLEET_EXAMPLE=ON
cmake --build --preset "runtime-$FLEET_LINKAGE" --config Release --target engine-sim-fleet
cmake --install "$ES_OUT/runtime-$FLEET_LINKAGE" --prefix "$FLEET_PREFIX" --config Release --component FleetSDK
cmake --install "$ES_OUT/runtime-$FLEET_LINKAGE" --prefix "$FLEET_PREFIX" --config Release --component FleetSDKExamples
```

`FleetSDK` contains the public header, package metadata, library and notices.
`FleetSDKExamples` adds the independent consumer project and two MIT models
with synthetic impulses.

## Run the installed hosts

From a workspace with the installed prefix, set `FLEET_PREFIX` to its
absolute path and `FLEET_BUILD` to a fresh absolute consumer build path. Run
this for each linkage being integrated.

```sh
examples="$FLEET_PREFIX/share/engine-sim-fleet/example"
cmake -S "$examples" -B "$FLEET_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$FLEET_PREFIX" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
  -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
cmake --build "$FLEET_BUILD" --config Release
suffix=
if [ "${OS:-}" = Windows_NT ]; then
  suffix=.exe
  export PATH="$FLEET_PREFIX/bin:$PATH"
fi
python3 -B "$examples/verify.py" \
  "$FLEET_BUILD/engine-sim-fleet-c-host$suffix" \
  "$FLEET_BUILD/engine-sim-fleet-cxx-host$suffix" \
  "$examples/fixture-single/main.json" "$examples/fixture-heavy/main.json"
```

The hosts run one FREE engine and one independently controlled DRIVEN
engine. The verification checks that both stems advance and contain signal,
then checks stop completion and retirement. It exercises the experimental
interface with a simulated device-free host.

## Set up the host and callback

1. On the coordinator, call `es_rt_api_version` and initialize the size and
   ABI fields of every versioned record. Create an owner with an unused
   nonzero epoch, rate, profile, maximum span and lane count. Use
   `ES_RT_EXPERIMENTAL` with qualification ID zero for the examples.
2. Prepare each voice from a JSON document with an absolute asset root and
   its own seed, mode and initial controls. Release unsubmitted tokens with
   `es_rt_discard`. Submit frame-zero activations before `es_rt_start`.
   Accepted activation or replacement commands transfer the candidate to
   the owner. Start primes the worker before the first audio callback.
3. On one audio thread, call only `es_rt_process` using preallocated,
   disjoint mono planes and the host's span up to the configured maximum.
   The host may request the mix, stems, or both. Keep controls, model
   preparation, polling, logging and storage changes on the coordinator.
4. Drain command results and returned cells on the coordinator. End the
   backend's callbacks and observe their completion before quiescing and
   collecting the final cells. Destroy after exclusive ownership returns.
   For a restart, create and prime a fresh epoch.

The audio call does bounded copying and fallback initialization for the
requested planes and span. Positive spans cannot exceed the configured
maximum of 64 or 128 frames in Interactive, or 64, 128, 256, 512 or 1024
in Balanced. Those values are structural options; the actual host and
models determine attainable timing. The library uses 64-frame producer
tiles and at most eight logical lanes. Replacement temporarily charges both
voices. Prepare at most two pending candidates and reap retirements before
reusing their capacity.

A valid broken call reports the supplied prefix in `pcm_frames`, fills
the suffix with zeros and advances submission over both regions. The first
fallback frame is `first + pcm_frames`. Invalid calls preserve output.
Recover on the coordinator after the backend barrier. The test host waits
for published frames before requesting them. A device supplies its own
cadence, which can expose a missed period or broken transport.

## Schedule controls and measure latency

Submit commands in increasing frame and sequence order. Use the fleet epoch
and monotonically increasing lane generations. Queue acceptance reserves a
result ticket. Read the ticket for application or failure and the lane
snapshot for physical latching. A physical command applies at the eligible
model tick start. Gain and lifecycle envelopes advance in output frames.
Supply demand, load in N·m and the DRIVEN shaft target in rpm as a physical
vector. The FREE engine has its own engine speed, ignition and starter.

At 48 kHz, one producer tile covers 1.333 ms. In the nominal Interactive
example at 128 frames, the low/high watermarks are 256 and 384 frames. The
tested fresh-control lookahead limit is 448 frames or 9.333 ms. The nominal
sample-to-callback-return budget is 576 frames or 12 ms. In Balanced at 256
frames, the watermarks are 768 and 1024. The corresponding limits are 1088
frames or 22.667 ms and 1344 frames or 28 ms. Hardware queue delay, model
response, DAC presentation and acoustic travel have separate measurements.

## Current admission and device limits

This release exposes experimental mode. With no verified admission row
installed, ordinary qualified creation returns `ES_RT_UNSUPPORTED`.
The device-neutral Linux benchmark exercises separate static and shared
binaries at 48 kHz with an exact two-vehicle workload and a documented
real-time scheduler. The experimental screens show workload and period
limits. Use the [measurement procedure](fleet-performance.md) for the exact
workload and independent stem checks.

Sustained qualification on a physical endpoint, native Windows and macOS
qualification, platform detector coverage and acoustic loopback remain
pending. Linux hosts can run the sustained matrix through PortAudio on an
explicit native ALSA output. The optional short live example is documented in
the [example guide](../examples/fleet/README.md#optional-live-host).
