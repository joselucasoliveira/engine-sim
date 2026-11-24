# Two vehicles with Fleet

The C11 and C++17 examples use the same public Fleet ABI and run a simulated
host without a device or window. Each starts an independent FREE or DRIVEN
engine, applies physical targets, consumes the mix and both stems, waits for
the stop receipts, and collects the retired voices.

Set `ENGINE_SIM_BUILD_RUNTIME_EXAMPLES=ON` in a product build with the runtime
enabled. Build the `engine-sim-fleet-c-host` and `engine-sim-fleet-cxx-host`
targets. Each accepts two distinct absolute model paths paired with their
absolute asset roots.

```sh
source_dir=/absolute/path/to/engine-sim
build_dir=/absolute/path/to/build
single="$source_dir/examples/runtime/fixture"
heavy="$source_dir/examples/fleet/fixture-heavy"
"$build_dir/bin/engine-sim-fleet-c-host" "$single/main.json" "$single" "$heavy/main.json" "$heavy"
"$build_dir/bin/engine-sim-fleet-cxx-host" "$single/main.json" "$single" "$heavy/main.json" "$heavy"
```

The simulated coordinator waits for enough published PCM before each
call. A device supplies its own cadence. Its callback updates the requested
frame count and calls `es_rt_process` with the prepared arrays. The host
drains callbacks before quiescing or destroying the fleet. The examples
use the experimental profile and report frame counts and signal presence.

The inputs are the MIT Runtime Single and Runtime Single Heavy Flywheel.
The latter preserves the original fixture and eight-sample synthetic
impulse with crank inertia set to 0.18 kg·m². See the [fixture license](fixture-heavy/LICENSE).
The compact authored model expands to the canonical fixture whose SHA-256
is `0dc441e0f9a8d2b740e7fe44d8a2371023f44184a96f972c57dbf49ea531507f`.

With `FleetSDKExamples` installed, configure its `example` directory
against the Fleet SDK prefix. Pass `fixture-single/main.json` and
`fixture-heavy/main.json` together with their containing directories
from that installed project. See [SDK installation](../../BUILDING.md)
when working from a product checkout.

## Optional live host

In the standalone project, set `ENGINE_SIM_FLEET_PORTAUDIO=ON` to build
`engine-sim-fleet-live`. Provide a system or separately installed shared
PortAudio v19 package. The reference provider version is 19.7.0. On Unix
the project also accepts the provider's `portaudio-2.0` pkg-config metadata.
PortAudio carries its own upstream license. The two SDKs and the ordinary
examples have no device dependency.

```sh
cmake -S /absolute/sdk/share/engine-sim-fleet/example -B /absolute/live-build \
  -DCMAKE_PREFIX_PATH=/absolute/sdk -DENGINE_SIM_FLEET_PORTAUDIO=ON
cmake --build /absolute/live-build --config Release
/absolute/live-build/engine-sim-fleet-live DEVICE_INDEX 48000 256 \
  /absolute/fixture-single/main.json /absolute/fixture-single \
  /absolute/fixture-heavy/main.json /absolute/fixture-heavy
```

Select a physical output index explicitly. The host opens float mono first,
then tries stereo duplication if the endpoint requires it. It checks the
opened rate and tracks callback periods, xruns, Fleet fallback and bounded
timestamp data. Device callbacks have no logging, polling or waits.
A valid abort buffer is initialized. Above the separate 1,024-frame adapter
ceiling, the emergency path silences the host-provided buffer and aborts.
The device cadence is part of native qualification.

Shutdown closes the backend before quiescing and collecting Fleet objects.
An optional `--recover-once` repeats device negotiation and preparation with
a fresh epoch after a discontinuity. The diagnostic reports the backend's
estimated DAC time and host-clock calibration. Physical latency requires a
loopback capture.
