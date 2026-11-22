# Runtime C ABI version 1

The [runtime header](../include/engine_sim/runtime.h) defines the version 1
contract for C11 and C++17 hosts. The build provides the static
`EngineSim::Runtime` target and declaration/layout checks. Installed SDK
packaging is separate from this build target.

## Platform and compatibility

The ABI uses natural alignment, eight-bit bytes, little-endian storage and
IEEE binary32 and binary64. Its initial native targets are 64-bit Linux x86,
Windows x64 with MSVC and Darwin arm64. Windows uses cdecl. Static Windows
consumers define `ES_RUNTIME_STATIC`. Shared consumers use the header's import
declarations, and the library build supplies its own export definition.

Version 1.0 packs its version as `0x00010000`. A library accepts requests with
the same major and a minor version up to its own. Field offsets, status values
and function signatures are stable across compatible versions. A later minor
may append fields whose absence has defined defaults. Check the capability
bits for features used by the host.

## Records and caller storage

Initialize the `size` and `abi` fields before passing an extensible record. The
size is the capacity in bytes, including any tail beyond the v1 prefix.
The minimum v1 sizes for configuration, controls and telemetry are 40, 40 and
136 bytes, respectively. A v1 call ignores an unknown input tail and preserves
the caller's output tail. Keep natural alignment and supply accessible,
nonoverlapping storage.

The library owns each opaque handle until `es_destroy`. The caller owns its
records, destination PCM, output counts and diagnostic storage. Samples are
mono floats. A render request supplies capacity in frames and has a maximum of
65,536 frames. The written count identifies exactly the valid output prefix.
Paths are borrowed, NUL-terminated UTF-8 during load. Serialize calls for a
single handle, including destruction.

## Status and diagnostics

Status values are signed 32-bit integers. `ES_OK` reports success.
`ES_INVALID`, `ES_VERSION`, `ES_UNSUPPORTED` and `ES_CAPACITY` identify input
and contract failures. `ES_STATE` identifies a call invalid in the current
handle state. `ES_SCRIPT`, `ES_RESOURCE` and `ES_MODEL` identify failures in
script execution, input resources and model preparation. `ES_RENDER_FAULT`
identifies a rendering fault. `ES_NO_MEMORY` and `ES_INTERNAL` cover allocation
failure and unexpected library errors.

A caller may pass an optional 512-byte `es_error`, including before a handle
exists. Each call that accepts an error record clears the message on success
or writes a bounded, NUL-terminated UTF-8 diagnostic on failure. Retain a
diagnostic by using different error storage for subsequent calls.

## Reset and independent instance lifetimes

The C interface in `engine_sim/runtime.h` uses library-owned opaque handles and caller-owned records, PCM and error storage. Query `es_capabilities()` for the features provided by a binary. A version number describes the ABI; it does not replace feature negotiation.

Each successful load owns the resolved scripts and impulse-response bytes actually consumed during preparation. The three absolute UTF-8 path arguments are borrowed only for the call. The runtime retains its own recipe and input identities. Loading a replacement is transactional: a preparation failure preserves the active engine, time, pending controls and previous fault state.

`es_reset` prepares a fresh candidate using the original configuration, seed and saved recipe. Each opened script and impulse response must resolve to the same canonical path and contain exactly the original bytes. Verification precedes parsing or decoding, and the complete input set must match before replacement. Missing, changed or unexpectedly resolved inputs return `ES_RESOURCE`, including changed malformed files. Restore the inputs to reset the existing definition, or call `es_load` explicitly to accept a new definition.

A successful reset reproduces a fresh load on the same binary and floating-point environment: prepared phase, filters, random streams, zero counters and default controls. Reset discards the former timeline and returns a faulted engine to READY. An EMPTY handle has no recipe to reset and returns `ES_STATE`. Unload releases the prepared session and recipe without filesystem reads; repeated unload and destroying NULL are valid.

Serialize every operation on one handle, including telemetry, reset and destruction. Transfer an instance between threads only at a synchronized quiescent boundary. When `ES_CAP_PARALLEL_RENDER` is present, distinct handles can render concurrently while other independent handles are prepared or destroyed. Parser preparation is serialized internally; rendering has no process-wide render lock. Never race calls on one handle or reuse a handle after destruction.

Each call accepts optional independent `es_error` storage. Successful calls clear only the supplied error message. Other handles and calls using different storage do not modify it. A rendering fault publishes only completed frames and preserves the rest of the caller's PCM buffer. Telemetry retains the last completed-frame snapshot with FAULTED state until successful load/reset or unload.

These are synchronous correctness and ownership guarantees. Rendering can allocate or lock internally and carries no audio-callback deadline guarantee. Deterministic equality requires the same binary, inputs, seed, control timeline and floating-point environment; it is not a cross-platform byte guarantee.

## Output rates and clock boundaries

Mono float output supports 44100 and 48000 Hz. The native synthesis/filter/IR stream remains 44100 Hz and retains its S16 quantization. At 44100 Hz every returned sample is the native S16 value divided by 32768 exactly. At 48000 Hz the output uses causal linear interpolation of the two latest native values, initialized from the first real native sample; it adds one native interval of delay. It preserves DC and pitch but attenuates high frequencies, and does not reconstruct lost native precision.

Frame n is at n/output_hz. A render call processes a half-open interval, completing all due physics ticks at its right boundary while deferring a coincident next native sample or control latch. With N completed output frames, telemetry reports floor(N*simulation_hz/output_hz) completed ticks and ceil(N*44100/output_hz) generated native frames. Controls latch at numerical tick starts. Smaller calls, including single frames, preserve the same physical timeline and PCM; requesting native audio never advances physics past the host call boundary.

Source interpolation contributes one numerical-tick pipeline delay; 48000 Hz output additionally contributes one native interval. Model/filter response and host device buffering are separate. There is no independent advance/drain operation or synthetic starvation padding in the runtime.

## Free-running shaft load

`ES_CAP_LOAD` enables `load_nm` from 0 through 10000 N·m. This is the maximum torque of a separate dry brake on output crank zero, targeting zero angular velocity. It opposes either direction, can hold incipient rotation at rest, and preserves the model's inherent friction. Public FREE sessions keep the transmission neutral, clutch disengaged and dynamometer disabled. Demand still drives the model's direct linkage or governor; ignition and starter remain independent controls.

`requested_load_nm` reports the latest accepted vector immediately. `load_reaction_nm` reports the last completed tick's actual signed reaction in counterclockwise N·m, initially zero. The reaction can be smaller than the request near rest, and an accepted request can await the next eligible tick while the old reaction remains visible. Negative, nonfinite or excessive loads reject the entire vector without changing pending controls. Reset restores zero load and reaction. This reaction is a brake measurement, not a combustion or net engine torque estimate.

## Commanded clockwise rotation

`ES_CAP_DRIVEN` enables creation in `ES_DRIVEN` mode. Set `driven_rpm` from zero through `driven_limit_rpm`, the smaller of 12000 RPM and the loaded model's redline. Starter torque overrides and requested starter activation are unsupported in this mode. Create another handle to change modes. Demand and ignition retain their model behavior.

A bilateral solver constraint drives output crank zero clockwise with at most 1000000 N·m. It integrates crank, cam and piston motion through the ordinary model; it does not assign speed or reset phase. Successful ticks have signed speed error at most 0.1 RPM. At positive commanded speed the requested load is a counterclockwise applied torque; at zero command its applied reaction is zero and the drive holds position against model forces. No additional standstill brake competes with the drive.

`applied_driven_rpm`, `drive_reaction_nm` and `load_reaction_nm` describe the last completed tick, initially zero. Reactions are counterclockwise positive. They describe the drive and load, not combustion torque. A valid abrupt speed change can exceed actuator authority: saturation, nonconvergence, excess speed/phase error or gross joint drift returns `ES_RENDER_FAULT` with only the completed PCM prefix. Reset or load recovers the handle. Smaller host-authored steps can reduce acceleration demand.

Local tests exercise the single-cylinder fixture, Kohler, LS and EJ25 with a settled 600 RPM start, ramp to 1800, hold, 1200 step and stop; gradual trajectories also reach their respective redlines. The Kohler's settled 0-to-3600 RPM step with 100 N·m load saturates and faults as expected. Direct large commands immediately after preparation can also saturate; these are measured operating limits, not a guarantee that every in-range step succeeds. Geometry checks bound crank-center and linkage separation to 0.003 times rod length, with a 1 nm roundoff floor, while retaining the model's ordinary numerical residuals.

The optional [offline renderer and ctypes example](runtime-render.md) demonstrate scheduled public API use and exact float WAV output without a device.
