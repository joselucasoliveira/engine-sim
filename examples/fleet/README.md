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
