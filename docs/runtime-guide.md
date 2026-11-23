# Embedding Engine Simulator

The runtime SDK loads a canonical engine JSON document and renders mono PCM synchronously through
thirteen C functions. It starts no window, audio device or worker. C11, C++17 and
foreign-language hosts use the same [public header](../include/engine_sim/runtime.h).
The SDK package is version 2.0.0 and the ABI is major 2, minor 0. Its mechanical and
acoustic model remains an exploratory simulator, unsuitable for engineering
validation or tuning decisions.

Rebuild clients for ABI 2 before linking the SDK. An ABI 1 client used the
legacy script load entry point. ABI 2 exposes explicit JSON file and byte
loads and detailed diagnostics. Pin the v2 CMake package and library name
when updating a dynamic client. See [platform and compatibility](runtime-abi.md#platform-and-compatibility).

## Build a code SDK and its optional example

Start in a checkout with the [native prerequisites](../BUILDING.md) installed.
Use CMake 3.25+, Ninja, a C++17 toolchain and the pinned solver submodule. The
C examples need a C11 compiler. These commands use a POSIX shell.

### Install the code-only SDK

Choose a fresh absolute output directory outside the checkout. For a
disconnected build, make the pinned solver Git object available first.

```sh
set -eu
git submodule update --init -- dependencies/submodules/simple-2d-constraint-solver
export CMAKE_GENERATOR=Ninja
export ES_OUT="${ES_OUT:-$(cd .. && pwd)/engine-sim-runtime-output}"
for variant in static shared; do
  cmake --preset "runtime-$variant"
  cmake --build --preset "runtime-$variant" --config Release --parallel 2
  cmake --install "$ES_OUT/runtime-$variant" --config Release \
    --prefix "$ES_OUT/sdk-$variant" --component RuntimeSDK
done
```

The default installation contains libraries, `engine_sim/runtime.h`, CMake
metadata, the [v1 schema](engine-config.md) and notices. The optional example
adds C and renderer executables, a synthetic single-cylinder JSON model and
its eight-sample impulse. The fixture's model and impulse are under MIT.
See the [notices](../THIRD_PARTY_NOTICES.md) and
[fixture description](../examples/runtime/fixture/README.md).

### Opt in to the synthetic example

With the same `ES_OUT`, install to a separate prefix. Set `VARIANT=shared`
before running this block to get the dynamic-library example.

```sh
export VARIANT="${VARIANT:-static}"
export SDK_PREFIX="$ES_OUT/example-$VARIANT"
cmake --preset "runtime-$VARIANT" -DENGINE_SIM_INSTALL_RUNTIME_EXAMPLE=ON \
  -DENGINE_SIM_BUILD_RUNTIME_EXAMPLES=ON
cmake --build --preset "runtime-$VARIANT" --config Release --parallel 2
cmake --install "$ES_OUT/runtime-$VARIANT" --config Release \
  --prefix "$SDK_PREFIX" --component RuntimeSDK
export FIXTURE="$SDK_PREFIX/share/engine-sim-runtime/example"
```

### Build the installed clients

Start in the source checkout with `SDK_PREFIX` and `FIXTURE` from the
example above. You can also use a code-only prefix and specify a caller-owned
model directory as `FIXTURE`. After relocating a prefix, set both paths to
their new absolute locations and configure the clients in a fresh build
directory.

```sh
export CONSUMER_BUILD="$ES_OUT/client-${VARIANT:-static}"
cmake -S examples/runtime/consumer -B "$CONSUMER_BUILD" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build "$CONSUMER_BUILD" --config Release --parallel 2
"$CONSUMER_BUILD/runtime-c-consumer" "$FIXTURE/main.json" "$FIXTURE"
"$CONSUMER_BUILD/runtime-cxx-consumer" "$FIXTURE/main.json" "$FIXTURE"
```

Consumers enable C and CXX, call `find_package(EngineSimRuntime 2.0 CONFIG REQUIRED)`
and link `EngineSim::Runtime`. The imported target supplies only installed usage
requirements and private archive dependencies. A C source uses only
`<engine_sim/runtime.h>`; a static consumer needs a C++ final link, as in the
[complete consumer CMake project](../examples/runtime/consumer/CMakeLists.txt).
Avoid loading an SDK implementation into a process already linked to another
copy of the runtime.

On Windows use a native VS 2022 x64 developer environment. PowerShell sets the
variables with `$env:ES_OUT`, `$env:SDK_PREFIX` and `$env:CONSUMER_BUILD`; use
`py -3` for Python and check `$LASTEXITCODE` after each native command. The recipe
selects Ninja, whose executables are in the consumer build root. Visual Studio
multi-config consumers instead put them under `Release`; retain `--config Release`.
Put the shared prefix's `bin` directory on `PATH` before running its clients.
The SDK uses the shared MSVC CRT. On macOS use native arm64 Xcode tools. ELF/Darwin clients need an application rpath or
their ordinary loader search path when moved. These prepared recipes do not
replace the [per-platform execution status](platforms.md#device-free-runtime-sdk).

## Build and load an engine from C++ values

`ConfigSDK` installs the C++17 headers and static archives. Use the same compiler,
standard library and build configuration for the SDK and its clients.
`find_package(EngineSimConfig 1.0 CONFIG REQUIRED COMPONENTS Config)` exposes
`EngineSim::Config` for typed values, validation and normalized topology.
The `Json` component exposes `EngineSim::Json` for canonical serialization
and parsing. The Config-only producer uses `-DENGINE_SIM_CONFIG_JSON=OFF`.

For a standalone model and JSON SDK, run the following with an external
`ES_OUT`. The [example](../examples/runtime/programmatic/model.cpp) validates
a typed single-cylinder engine, prints the bad journal location, repairs it
and writes canonical JSON.

```sh
export CONFIG_PREFIX="$ES_OUT/config-sdk"
cmake --preset config -DENGINE_SIM_INSTALL_CONFIG_SDK=ON
cmake --build --preset config --config Release
cmake --install "$ES_OUT/config" --config Release --prefix "$CONFIG_PREFIX" --component ConfigSDK
cmake -G Ninja -S examples/runtime/programmatic -B "$ES_OUT/model-client" \
  -DCMAKE_PREFIX_PATH="$CONFIG_PREFIX" -DENGINE_SIM_PROGRAMMATIC_RUNTIME=OFF
cmake --build "$ES_OUT/model-client" --config Release
"$ES_OUT/model-client/engine-sim-model" "$ES_OUT/typed-model.json"
```

For a client that also renders, install both components from the same producer
into one prefix. A static build shares the Config and Json archives in that
prefix. To use the shared runtime, use `runtime-shared` and a separate prefix.
The C ABI remains the binary interface for embedding.

```sh
export SDK_PREFIX="$ES_OUT/programmatic-sdk"
export PROGRAMMATIC_CLIENT="$ES_OUT/programmatic-client"
cmake --preset runtime-static -DENGINE_SIM_INSTALL_CONFIG_SDK=ON \
  -DENGINE_SIM_INSTALL_RUNTIME_EXAMPLE=ON
cmake --build --preset runtime-static --config Release
cmake --install "$ES_OUT/runtime-static" --config Release --prefix "$SDK_PREFIX" --component RuntimeSDK
cmake --install "$ES_OUT/runtime-static" --config Release --prefix "$SDK_PREFIX" --component ConfigSDK
cmake -G Ninja -S examples/runtime/programmatic -B "$PROGRAMMATIC_CLIENT" -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build "$PROGRAMMATIC_CLIENT" --config Release
export FIXTURE="$SDK_PREFIX/share/engine-sim-runtime/example"
"$PROGRAMMATIC_CLIENT/engine-sim-programmatic" "$FIXTURE" 44100 free \
  "$ES_OUT/model.json" "$ES_OUT/typed.f32"
"$PROGRAMMATIC_CLIENT/engine-sim-file-loader" "$FIXTURE/main.json" "$FIXTURE" 44100 free \
  "$ES_OUT/file.f32"
cmp "$ES_OUT/typed.f32" "$ES_OUT/file.f32"
```

Both programs accept 44100 or 48000 and `free` or `driven`. They write two
seconds of mono PCM as native-endian float32 samples, report telemetry and
reset the same engine. The C++ example passes an owned string to
`es_load_json_bytes`. Serialization allocates the string and the load parses,
validates and pins its own copy with the explicit asset root. The caller can
release the string as soon as the load returns. File loads pin the entry
identity and reject changed input bytes on reset.

For a script, use a separately built [legacy converter](legacy-converter.md).
This example converts the licensed single-cylinder fixture. Set
`CONVERTER` to the absolute path to `engine-sim-convert`.

```sh
export SCRIPT_FIXTURE="$PWD/examples/runtime/fixture"
"$CONVERTER" --entry "$SCRIPT_FIXTURE/main.mr" --library-root "$SCRIPT_FIXTURE/library" \
  --asset-root "$SCRIPT_FIXTURE" --resource-root "$SCRIPT_FIXTURE" \
  --output "$ES_OUT/converted.json" --manifest "$ES_OUT/conversion.json"
"$PROGRAMMATIC_CLIENT/engine-sim-file-loader" "$ES_OUT/converted.json" "$SCRIPT_FIXTURE" \
  44100 free "$ES_OUT/converted.f32"
```

## Complete C usage and exact event times

The [C11 consumer](../examples/runtime/consumer/consumer.c) includes initialization,
error handling, loading, controls, caller-owned `float[257]`, telemetry and
destruction. It renders exactly 88,200 frames at 44,100 Hz. Ignition and starter
begin enabled with demand 0.25; the call at frame 44,100 releases the starter.
Every request is split at that event and the final frame. The
[C++17 consumer](../examples/runtime/consumer/consumer.cpp) uses RAII and renders
96,000 frames at 48,000 Hz with the same one-second event. Neither plays PCM.

Initialize configuration with `es_config_init`, then edit supported fields before
`es_create`. Defaults are 44,100 Hz, FREE mode, seed 1, model numerical frequency,
zero flags and the model's starter torque. The handle begins EMPTY; a successful
`es_load_json_file` makes it READY at frame zero. Set every field of a complete controls
record, including its size and ABI. The [ABI reference](runtime-abi.md) defines
all ranges, units, states and diagnostics. Driver demand follows the preset's
direct linkage or governor; it is not a measured throttle angle.

For an event at absolute output frame E, render only up to E, submit the new
vector, then render frame E onward. Controls latch at numerical tick starts.
A change between starts waits for the next start, less than one numerical tick;
the last accepted vector before that start wins. Preserve this absolute control
timeline when changing block sizes. The runtime has no wall clock or event queue.
Pausing means making no render calls; resuming retains phase and histories.

`es_render` accepts at most 65,536 frames and returns the actual `written` count.
Check status and consume only that prefix, including on a computation fault.
The caller's suffix remains untouched. Preflight errors advance nothing; a
faulting interval does not advance the public counters. The runtime never pads
missing source frames with silence. A host can report a device deficit and add
its own silence without claiming the source rendered those frames.

Pass an absolute UTF-8 JSON entry and asset directory to
`es_load_json_file`. `es_load_json_bytes` accepts a borrowed length-delimited
document and the same explicit asset directory. The load owns the JSON and
the referenced impulse-response data after return. Resolve relative asset
locators beneath that directory. Use the offline converter to migrate an
existing script. [Engine migration guide](engine-migration.md)

Failed load/reset preserves the active graph, counters, controls and fault state.
Reset can access the filesystem: it requires the exact original canonical paths
and bytes for every consumed input before replacing the session. Use an explicit
load to adopt edited files. Successful reset restores the original seed, prepared
phase, filters, default controls and zero counters. Unload is idempotent and
clears the recipe. Destroy through `es_destroy` only; NULL is accepted. To change
seed, output rate, mode or numerical-rate override, create another handle.

Serialize all calls on one handle, including telemetry and destruction. A
different thread can take ownership only at a synchronized quiescent boundary.
Distinct prepared handles can render concurrently and keep independent clocks,
controls and random streams.
Synchronous rendering can still allocate or lock; an audio callback deadline
has not been qualified. A host worker with bounded ready PCM is an integration
choice, not a service provided by this API.

### Reproduce reset recovery

With the shared example installed, run the recovery example from the
checkout. It makes a private disposable copy of the synthetic engine and
its impulse before trying a malformed reload and two changed-input resets.

```sh
python3 -B examples/runtime/reset_recovery.py \
  --library "$SDK_PREFIX/lib/libengine-sim-runtime.so.2" --fixture "$FIXTURE"
```

The malformed reload reports `CFG_SHAPE`. The changed JSON and changed
impulse report `CFG_RESOURCE_BYTES` and leave the running timeline intact.
Restoring both exact files allows reset, and the next PCM block matches
a second independently prepared handle. Use the installed DLL or dylib
path shown below on Windows or macOS.

## Render a WAV or load the shared ABI from Python

With either example-enabled SDK above, from the checkout:

```sh
export AUDIO_OUT="$ES_OUT/offline-run"
mkdir -p "$AUDIO_OUT"
printf 'frame\tdemand\tload_nm\tignition\tstarter\tdriven_rpm\n0\t0.25\t0\t1\t1\t0\n48000\t0.25\t0\t1\t0\t0\n' > "$AUDIO_OUT/run.tsv"
"$SDK_PREFIX/bin/engine-sim-render" --entry "$FIXTURE/main.json" \
  --asset-root "$FIXTURE" --rate 48000 \
  --mode free --seed 1 --frames 96000 --schedule "$AUDIO_OUT/run.tsv" \
  --out "$AUDIO_OUT/run.wav"
```

The TSV contains real tabs. The completed file has exactly 96,000 mono IEEE
float32 little-endian frames, a WAVE format tag of 3 and a matching fact count.
The destination must not already exist. Invalid schedules, runtime faults or I/O
errors fail without publishing partial audio. See the
[offline tool reference](runtime-render.md) for repeated events, limits and errors.
No device is opened.

For a shared Linux SDK:

```sh
python3 -B examples/runtime/ffi_smoke.py \
  --library "$SDK_PREFIX/lib/libengine-sim-runtime.so.2" \
  --entry "$FIXTURE/main.json" --asset-root "$FIXTURE"
```

Use `bin/engine-sim-runtime-2.dll` on Windows or `lib/libengine-sim-runtime.2.dylib` on macOS.
The [ctypes example](../examples/runtime/ffi_smoke.py) sets cdecl signatures and
record layouts for all thirteen functions and performs a two-second 48 kHz run,
reset, unload and destroy with caller-owned storage.

## Capture simulation diagnostics

The offline capture tool reads JSON models from the resource directory and
records the exact model and response bytes used during preparation. The
mechanics suite writes a per-tick shaft trace, combustion events, audio queue
counters and native PCM. Run it from any directory and write to a fresh
output outside the checkout and installation.

```sh
engine-sim-capture run --suite mechanics --case MECHANICS-KOHLER \
  --resources "$RESOURCE_ROOT" --out "$CAPTURE_OUT" --seed 209 \
  --repetitions 1 --mode sync
```

Use `--suite reference --case LOAD --model "$MODEL_JSON"` to inspect a
different canonical model. Pass its absolute asset directory to
`--resources`. The test also records rejected syntax, schema and missing
resource cases. If the executable is installed, omitting `--resources`
uses its adjacent `share/engine-sim` directory. Convert old scripts with
the offline tool before capture. [Legacy converter guide](legacy-converter.md)

## Signal, desktop and compatibility limits

The native DSP remains mono 44.1 kHz, with the original noise/filter/convolution,
gains, asymmetric S16 saturation and quantization. Public 44.1 kHz samples are
native S16 divided by 32,768. The 48 kHz path interpolates the two latest native
values in binary64 and rounds once to float32; it does not create native precision.
The range is [-1, 32767/32768]. Causal exhaust reconstruction contributes one
numerical tick of delay; 48 kHz adds one native sample interval. Model/filter
response and device buffering add their own delay. The inherited exhaust-delay
tap design still uses 10,000 Hz even for other numerical rates.

An isolated converter test used rounded S16 sine input of amplitude 0.7, one
second of warm-up and a two-second phase fit. Its measured gains relative to the
native input were -0.014607 dB at 997 Hz, -0.950723 dB at 8 kHz, -3.939100 dB at
16 kHz and -6.335330 dB at 20 kHz, consistent with causal linear interpolation.
These describe the converter, not the complete engine, IR or physical device.

Independent random streams, causal endpoint reconstruction and exact half-open
frame counting intentionally change legacy waveforms. A legacy inclusive endpoint
can add one sample. Compare matched physical intervals and model/spectral
invariants rather than demanding old PCM hashes. Same-binary, same-input/seed,
same-control-timeline renders are bit-identical across request partitions and
reset on the same floating-point environment. Cross-platform byte identity and
improved acoustic fidelity are not promised.

The desktop uses the private bridge to the same computation, with its existing
gear, clutch, dyno, mixer, scope and numerical-frequency controls. Public FREE
mode keeps a neutral disengaged driveline and dyno off; its load is a separate
dry brake. Public DRIVEN mode imposes a clockwise shaft trajectory through the
solver. It does not substitute a tone generator or overwrite shaft phase. See
[tracking limits and overload recovery](runtime-abi.md#commanded-clockwise-rotation).

Desktop pacing fills a 100 ms target from actual device progress and writable
capacity, counting every ready or reserved frame once. No demand means no new
source frames. Partial writes retain their ordered suffix; stopped devices keep
the queue bounded. Presentation FPS does not advance simulation. Slow-motion
transport retains fractional phase through ratio changes; pause keeps runtime
time stationary and manual steps have their own presentation cursor. Successful
reload quiesces submission, replaces views/session and resets host transport;
failed preparation retains the live generation. Adapter pause/underrun silence
is counted separately from runtime output. This differs from the legacy
frame-feedback producer that could overfill synthesis queues at idle.

Linux fake-host, transport, pause/reload and private native capture checks cover
those contracts. A user's positive EJ25 idle listening check is subjective
evidence; it does not supply new physical loopback counters, latency or complete
hardware qualification. [Platform status](platforms.md) keeps those limits separate.

## Measured cost and next integration work

A local Linux x86_64 Release study used GCC 14.2 on an Intel Core Ultra 9 285K,
ordinary scheduler affinity across 24 logical CPUs and varying clock frequency.
It measured Kohler and LS at 44.1/48 kHz, blocks 1/256/1024 and one/two independent
owners: 24 configurations with ten alternating-order pairs each. Each run warmed
two simulated seconds and measured [2, 2.25) seconds. Times below are elapsed
milliseconds for that quarter-second, for one owner, 44.1 kHz, blocks of 256.

| Model | Public median | Q1–Q3 | Minimum–maximum |
| --- | ---: | ---: | ---: |
| Kohler | 84.972 ms | 84.604–85.002 ms | 83.847–85.366 ms |
| LS | 131.261 ms | 131.071–132.169 ms | 130.448–132.802 ms |

Across all configurations, paired public/private-core median ratios ranged
0.997–1.018 and public/legacy ratios 1.009–1.027. The separately rebuilt legacy
reference used matching optimization/PIC flags. It retained inclusive endpoint
counting and its own RNG, 441-frame internal blocks and driveline; its added
48 kHz conversion was timed explicitly. Two old owners required two processes,
while new owners used two handles/threads. Those ratios cannot assign a causal
cost to the clock, RNG or facade alone and do not establish callback deadlines.

Separate instrumented runs at both rates measured roughly 56–57 ms physics plus
29 ms native DSP for Kohler, and 74 ms plus 58 ms for LS per measured quarter-second.
Preparation in those probes took about 287–290 ms for Kohler and 361–364 ms for
LS; reset took 279–281 ms and 353–355 ms. Inputs were warmed for provenance;
first-in-process preparation is not a cold-filesystem benchmark. Timer pairs
averaged about 9.7–10.4 ns, separate from optimized runs. Four render probes
counted zero ordinary C++ new/new[] and zero wrapped static pthread lock calls;
direct C/aligned allocation and locks inside dynamic system libraries were outside
that counter. Inspecting source remains necessary before callback integration.

After 100 load/render/unload cycles, first-to-last current RSS grew 16 KiB for
Kohler and 12 KiB for LS. This is allocator retention evidence, separate from
passing leak-sanitizer tests. CPU timings and RSS are workload/host observations;
native Windows/macOS cost measurements remain pending.

The next integration work needs measured deadline distributions for each model,
small-block and multiple-owner budgets, physics and convolution/DSP hotspots,
preparation caching that retains exact reset input identity, snapshot/conversion
cost, and a bounded host worker/device queue. Strict callback guarantees, a job
scheduler, many-vehicle budgets, acoustic LOD, new sound synthesis and plugin/game
integration are outside this SDK's current contract. See [tests](../TESTING.md)
and [native status](platforms.md#device-free-runtime-sdk) for reproducible checks.

## Measure preparation and render costs

The runtime benchmark can use an independent Release consumer linked through
`EngineSim::Runtime`. Start with a code-only install from either runtime SDK
preset. The smoke workload uses the synthetic fixture and compares complete PCM
and clock counters across block partitions at both supported rates.

```sh
export FIXTURE="$PWD/examples/runtime/fixture"
export COST_BUILD="$ES_OUT/cost-consumer"
cmake -S test/runtime/cost-consumer -B "$COST_BUILD" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build "$COST_BUILD" --config Release --target runtime-cost
python3 -B tools/runtime_bench.py installed-costs --smoke \
  --installed "$COST_BUILD/runtime-cost" --fixture "$FIXTURE" \
  --out "$ES_OUT/cost-smoke"
```

Use a fresh output directory per run. On a multi-config generator select the
Release executable, and use the shared SDK loader setup described above.

`PreparationPhaseAccounting` records the file-read or byte-copy, tokenization
and parsing, schema validation, normalization, semantic graph, resource I/O and
decoding, mechanics, synthesis and activation costs in a separate instrumented
process. Inclusive totals contain nested phases. Its allocation accounting
covers ordinary C++ allocations, with separate live counts for immutable shared
tables and per-session objects before and after replacement. The Linux lock
counter covers wrapped pthread mutex acquisitions. Run the bounded checks from
a configured and built `runtime-test` tree.

```sh
ctest --test-dir "$ES_OUT/runtime-test" -C Release --no-tests=error \
  --output-on-failure -R '^(PreparationPhaseAccounting|IsolatedRuntimeCosts)$'
```

`installed-costs --help` lists the independent reference inputs for the paired
Linux study. A timed row identifies its inputs, output digests and clock counts.
Report timings with the fixture, compiler, build profile, loader, rate, control
timeline and processor settings. These measurements cover synchronous rendering
and preparation on the measured setup.
