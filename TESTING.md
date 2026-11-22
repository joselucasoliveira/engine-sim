# Testing Engine Simulator

Run commands from a clean checkout and keep outputs outside it. Record `git rev-parse HEAD`, `git submodule status --recursive`, compiler/SDK versions, the configuration, first failing output and actual exit status. A successful configure or empty CTest result proves no runtime behavior. See [BUILDING](BUILDING.md) for prerequisites and [platform limits](docs/platforms.md).

## Fast numerical tests

```sh
export ES_OUT=/absolute/external/core-run
python3 -B tools/bootstrap.py --profile core --tests --out "$ES_OUT"
cmake --preset core
cmake --build --preset core -j 4
ctest --preset core --no-tests=error --output-on-failure
```

This selection currently contains 62 cases covering gas flow, functions, synthesis, queues, waveform parsing, resources, scene parsing and host contracts. Debug keeps internal assertions active. Release still uses explicit checks and GoogleTest assertions.

## Replay a revision

`tools/verify_build.py` runs the public bootstrap, preset, nonempty suite admission, analysis where applicable and private install. It requires a clean exact checkout and a new absolute output directory. It records commands, versions, test counts, archive identities, logs and digests under `OUT/logs`. `--allow-dirty` explicitly labels precommit evidence and cannot qualify a clean revision.

```sh
python3 -B tools/verify_build.py --preset core --expected-revision "$(git rev-parse HEAD)" \
  --expected-arch x86_64 --out /absolute/external/core-replay
```

Use `--cache ABSOLUTE_CACHE --offline` for a preloaded archive cache. `--reproducible-paths` maps source, dependency and output paths with GCC/Clang and records the maps; binary equality must still be measured. The tool records `SOURCE_DATE_EPOCH`, locale and timezone. See the workflows for prepared native runner recipes; defining a workflow does not establish a passing hosted run.

## Memory and thread diagnostics

Prepare the `capture` profile with tests for `asan` or `tsan`. Linux ASan uses Clang; TSan uses a separate GCC build. Use matching sanitizer runtimes and symbolizers. ASan runs with leak detection and UBSan stopping on the first violation; TSan runs separately. Discovery/build may disable leak detection while enumerating tests, but actual memory tests enable it.

```sh
python3 -B tools/verify_build.py --preset asan --cxx-compiler clang++ \
  --expected-revision "$(git rev-parse HEAD)" --out /absolute/external/memory-run
python3 -B tools/verify_build.py --preset tsan \
  --expected-revision "$(git rev-parse HEAD)" --out /absolute/external/thread-run
```

The headless memory selection has 100 cases; the threaded selection has seven. `desktop-asan` adds the SDL window, real GPU, audio and application tests, giving 120 cases including capture/resource checks. `desktop-tsan` selects 19 worker, transport and smoke-argument cases; whole-GPU instrumentation is a separate check. Use `--c-compiler clang --cxx-compiler clang++` for desktop ASan so SDL C sources are instrumented too. Windows `win-asan` requires the native MSVC component and remains pending.

On the exercised Linux X11 tuple, strict LeakSanitizer requires the explicit `--retain-xlib` condition to keep Xlib's process-global caches alive through exit. The verifier records the exact loaded library and hash. It does not suppress allocations or disable a checker. The default lavapipe worker TSan race and real-bus libdbus lock-order diagnostic are independently reproducible with SDL-only controls. They remain open external-runtime cells. A separate whole-app run with `LP_NUM_THREADS=0` passes without disabling the simulator or audio threads. For deterministic worker-only TSan without an optional desktop bus, set `DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/engine-sim-missing-control-bus` and record that condition. These controls do not qualify the default driver/bus tuple.

## Desktop and installed smoke

Direct `desktop` CTest currently has 116 Unix cases. The CI verifier also enables capture and resource-install checks, giving 120. A real drawable backend is required for renderer tests. On Linux automation, install Xvfb, xauth and a Vulkan implementation and explicitly select X11; otherwise an existing Wayland session may be selected.

```sh
export SDL_VIDEO_DRIVER=x11
# Set VK_DRIVER_FILES to the actual lavapipe ICD JSON installed on your host.
xvfb-run -a -s '-screen 0 2560x1440x24 -nolisten tcp' \
  python3 -B tools/verify_build.py --preset desktop --smoke --audio-mode virtual \
  --expected-revision "$(git rev-parse HEAD)" --out /absolute/external/desktop-run
```

Staged-resource tests require Linux bubblewrap or the native Windows/macOS equivalent used by their fixture. If unprivileged namespaces are unavailable, a trusted local runner may explicitly provide a JSON command prefix in `ENGINE_SIM_BWRAP_COMMAND`; the workflow records its narrow `sudo -n bwrap` fallback. Do not grant privileged execution to untrusted pull-request code.

Run an already installed application from an unrelated working directory:

```sh
python3 -B /absolute/installation/share/engine-sim/tools/run_desktop_smoke.py \
  --app /absolute/installation/bin/engine-sim-app \
  --script /absolute/installation/share/engine-sim/assets/main.mr \
  --out /absolute/external/installed-smoke --frames 240 --audio-mode virtual
```

The helper checks process exit and timeout, independent script/application hashes, actual frame readbacks and teardown. It requires ignition/starter/throttle, layout changes, resize, a successful Return reload and real solver/PCM progress. Its bounded internal pause fixture exercises a Right-key single step; this adds no interactive pause binding. Use `--audio-mode device` separately for a real endpoint. Virtual audio uses an explicitly selected software sink and never substitutes silently for a failed device. The source is mono signed 16-bit PCM at 44.1 kHz; SDL converts only at the final native boundary. Input-consumption counters are not physical playback or acoustic clocks.

Keep source-hidden relocated, missing/corrupt script, IR, font, scene and shader checks distinct. Failed reload must keep the running session; a later accepted reload must replace and retire it cleanly. The visible diagnostic regression covers spaces, UTF-8 and multiline messages with the info panel shown.

## Signal and timing comparisons

Bootstrap/build `capture` and use its executable from `OUT/capture/bin`. `RESOURCES` names the checkout or a complete prepared base resource tree. The characterization tool needs no GPU or audio device.

```sh
"$ES_OUT/capture/bin/engine-sim-capture" run --suite quick --resources "$RESOURCES" \
  --out "$ES_OUT/quick" --seed 1 --repetitions 1 --mode sync
"$ES_OUT/capture/bin/engine-sim-capture" run --suite reference --resources "$RESOURCES" \
  --out "$ES_OUT/reference" --seed 1 --repetitions 30 --mode sync
"$ES_OUT/capture/bin/engine-sim-capture" run --suite mechanics --resources "$RESOURCES" \
  --out "$ES_OUT/mechanics" --seed 1 --repetitions 1 --mode sync
"$ES_OUT/capture/bin/engine-sim-capture" calibrate --baseline "$ES_OUT/reference" --out "$ES_OUT/policy"
"$ES_OUT/capture/bin/engine-sim-capture" compare --reference "$REFERENCE" \
  --candidate "$CANDIDATE" --policy "$POLICY" --out "$ES_OUT/comparison"
```

Freeze reference bytes and policy before evaluating a change. Record source/build hashes, seed, scenario, inputs, sample format, block schedule and workload. Preserve startup, solver, synthesis, wall/CPU/RSS and queue metrics where observable. Calibration uses 20 runs and ten held-out runs. Do not widen a policy or turn an invalid parent's undefined output into a golden signal. Listening uses independently level-matched copies and remains separate from numerical integrity, device feeding and physical loopback evidence.

Capture seeds accept the full unsigned 64-bit range. Each engine owns separate
SplitMix64 streams for combustion in each cylinder, air noise in each exhaust
channel and jitter in each channel. Their domain numbers are 1, 2 and 3,
respectively, with zero-based topology indices. Loads initialize fresh stream
states. The host's libc random sequence is independent of rendering.

The engine streams replace the earlier libc and standard-library generator
phases. Cross-generation comparisons retain the original reference policy,
physical and timing invariants. A separate old-code acoustic profile samples
initial jitter states across seeds and exhaust channels. Freeze its sampling
choices and validate old-code holdouts before evaluating fresh candidate seeds;
retain the outcomes under the original fixed-jitter profile as well. Preserve
the executable, resources, calibration and holdout identities for each profile.
Within one executable and floating-point environment,
identical resources, seeds and controls must reproduce PCM and mechanical
traces across interleaving and parallel execution of independent engines.

The mechanics suite records the governed Kohler and direct-linkage LS from
prepared rest through starter release, fixed demand and ignition-off coastdown.
Its versioned files record crank speed (positive counterclockwise) and wrapped
four-stroke cycle angle (increasing clockwise) at each completed tick, the order
of cylinders that actually light, and queue counters after each 10 ms drain.
Combustion events correspond to successful lights. A spark that fails to light
the charge produces no event in this trace.

Legacy trace block `b` ends at `(b + 1)/100` seconds. Tick `k` ends at `k/F`
seconds for the script's simulation frequency `F`. Sample index `n` lies at
`n/44100` seconds. Compare at matching rational times rather than aligning by
per-block frame counts. A duration of `B/100` seconds has `441 * B` samples on
the half-open grid. The legacy synthesizer may also emit the endpoint sample,
which is retained in the raw reference. The seven reference cases and their
original trace columns remain available through `--suite reference`.


Desktop source ownership is exercised with `ctest --preset desktop -L '^r2-desktop$' --no-tests=error --output-on-failure`. These cases render through the private runtime owner, then exercise stopped/partial/wrapped device requests, live numerical-rate changes, control-timeline partition equivalence, staged reset/input identity, stale observations and real fault prefixes. The existing pacing and view suites remain separate. Automated native tests use the documented virtual audio and X11 conditions.

The desktop's `RT/dT` gauge measures time spent rendering source PCM, divided by completed physics ticks in that presentation interval. It includes solver, native DSP and runtime observation bookkeeping. A frame that produces no source does not invent a timing sample or advance the model. Queue occupancy, source faults, discarded frames and device starvation remain transport diagnostics; UI frame rate determines presentation only. This is a cost diagnostic, not an audio-callback deadline guarantee.

Desktop script rates retain their legacy conversion to an effective integer rate before checking the private 400..400000 Hz profile. Public runtime script rates remain integral in 4000..96000 Hz. Live desktop tuning finishes the old numerical tick before applying the new rate, retaining source and transport phase.

Engine characterization uses the same prepared session and synchronous runtime
renderer as the SDK and desktop. `ctest --preset capture -L '^r2-capture$'
--no-tests=error --output-on-failure` exercises this path. Each engine block
requests exactly 441 frames at 44100 Hz. Completed-tick diagnostics retain the
signed shaft velocity, cycle phase, successful lights and cylinder gas ranges;
output serialization runs after rendering. The capture owner bounds each block
to 10 ms, 961 tick observations and 64 cylinders. These diagnostic limits do not
change the public runtime profile.

Run metadata is version 2. Engine `capture-v2.json` identifies the half-open
clock and column meanings. `startup.tsv` records combined preparation time and
a reserved zero; `simulation_us` records combined runtime render time and
`synthesis_us` is reserved zero. The original columns remain readable, but do
not interpret these combined costs as separately profiled solver/DSP timings.
The mechanics queue trace reports actual runtime/native frames, zero legacy
input/output queues, and zero drops or source padding. Primitive FIR/GAS/TONE
kernels and QUEUE/THREAD stress retain their own implementation and endpoint
semantics. Stress reports name their scope: primitive ring, legacy synth worker
or shared runtime lifecycle. The `holdout` suite selects the Honda VTEC engine,
separate from the seven historical reference cases.

The causal engine clock changes endpoint interpolation and removes the final
inclusive engine sample: Kohler has 441000 frames, LS 529200 and EJ25 705600.
The independent legacy recordings retain their original extra sample. Preserve
original exact comparison failures and use the separately frozen physical and
acoustic migration policies for their applicable descriptors; a second wrapper
around the candidate cannot establish historical equivalence. Current-binary
calibration still requires 20 repeatable trials and ten holdouts, and rejects
missing, changed or malformed source artifacts. Such repeatability is separate
from an independent historical compatibility assessment.

Successful engine and lifecycle captures retain the exact prepared script/IR
bytes in `prepared-input-*.bin`, their canonical UTF-8 identities in the paired
`.path` files, and a versioned index. Fixture hashes use these owned bytes;
editing an original resource during rendering cannot relabel an earlier load.
Keep captures containing private scripts or uncleared IRs private. The legacy
`inputs.txt` remains a human-readable path list. Engine trace `input_phase` is a
reserved zero on the native half-open grid; queue fields are checked against
the synthesizer's actual counters and must show no pending audio, drops or
padding on successful captures.
