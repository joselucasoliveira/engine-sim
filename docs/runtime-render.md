# Offline rendering and foreign-language embedding

Build the optional native examples with `ENGINE_SIM_BUILD_RUNTIME_EXAMPLES=ON` in a `runtime-static` or `runtime-shared` SDK build. Install component `RuntimeSDK` into its own prefix. `engine-sim-render` uses only the public C interface and opens no audio device. The native tool has no Python dependency. The source at `examples/runtime` also builds as an independent CMake project against an installed `EngineSim::Runtime` target.

Supply all nine options:

```sh
engine-sim-render --entry "$MODEL/main.mr" --library-root "$MODEL/library" --asset-root "$MODEL" \
  --rate 48000 --mode free --seed 1 --frames 96017 --schedule "$OUT/run.tsv" --out "$OUT/run.wav"
```

Entry, library and asset paths must be absolute UTF-8 paths, as required by the C API. The schedule and output paths may be relative to the caller's directory. On Windows the native wide-character entry point preserves Unicode arguments, including output paths. Rates are 44100 or 48000 Hz; modes are `free` or `driven`. Seeds accept every unsigned 64-bit value.

The schedule is a UTF-8 TSV file with this exact header and six tab-separated fields per row:

```text
frame	demand	load_nm	ignition	starter	driven_rpm
```

Use real tab characters. For example:

```sh
printf 'frame\tdemand\tload_nm\tignition\tstarter\tdriven_rpm\n0\t0.25\t0\t1\t1\t0\n48000\t0.25\t0\t1\t0\t0\n' > "$OUT/run.tsv"
```

Rows use nondecreasing absolute unsigned frame indices less than `--frames`. The last complete row at a repeated frame replaces the entire vector. Every row is validated, including overwritten rows. Controls apply before rendering the named frame and persist until replaced; default controls apply before the first row. Demand is in [0,1], load is in [0,10000] N·m and booleans are zero or one. FREE requires driven RPM zero. DRIVEN requires starter zero and a nonnegative speed at or below the loaded model's reported limit. An abrupt valid speed request can still saturate the actuator and fault. See [the ABI guide](runtime-abi.md) for physical meanings and tick-latch timing. UTF-8 BOM and CRLF are accepted.

Output is mono IEEE float32 little-endian RIFF/WAVE, format tag 3, with a fact sample count and exact final partial block. The tool checks the 32-bit RIFF extent before rendering. It writes a unique temporary file beside the destination and publishes the completed file after successful rendering and close. Invalid input, runtime faults and I/O failures return a nonzero exit status; temporary audio is discarded. An existing destination is preserved and rejected. Zero frames with a header-only schedule produces a valid empty WAV.

`engine-sim-runtime-consumer` is the accompanying C11 example. Pass absolute entry, library and asset paths to render two seconds through caller-owned blocks. The independent consumer project under `examples/runtime/consumer` includes C11 and C++17 clients; static clients use the C++ final link.

`examples/runtime/ffi_smoke.py` demonstrates `ctypes.CDLL` with cdecl bindings for all eleven functions, native record sizes/offsets, caller-owned PCM and diagnostics before creation:

```sh
python3 examples/runtime/ffi_smoke.py --library "$SDK/lib/libengine-sim-runtime.so.1" \
  --entry "$MODEL/main.mr" --library-root "$MODEL/library" --asset-root "$MODEL"
```

Select the installed DLL or dylib path on other hosts. The script renders two seconds at 48 kHz, releases the starter after one second, inspects telemetry, resets, unloads and destroys the handle. It never frees a library allocation directly. Serialize calls to each handle; separate prepared handles may render independently. Library qualification is recorded per native platform, independently of these example sources.
