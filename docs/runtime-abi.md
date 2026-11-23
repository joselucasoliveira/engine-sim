# Runtime C ABI version 2

The [runtime header](../include/engine_sim/runtime.h) defines the version 2
contract for C11 and C++17 hosts. Static and shared SDK installations both
provide `EngineSim::Runtime`, with relative installed dependencies and only
the thirteen C exports in the shared library. Start with the
[integration guide](runtime-guide.md) for complete build and usage commands.

## Platform and compatibility

The ABI uses natural alignment, eight-bit bytes, little-endian storage and
IEEE binary32 and binary64. Its initial native targets are 64-bit Linux x86,
Windows x64 with MSVC and Darwin arm64. Windows uses cdecl. Static Windows
consumers define `ES_RUNTIME_STATIC`. Shared consumers use the header's import
declarations, and the library build supplies its own export definition.

Version 2.0 packs its version as `0x00020000`. A library accepts requests with
the same major and a minor version up to its own. An existing v1 client needs its
v1 header and library. Install v1 and v2 into separate prefixes. Field offsets, status values
and function signatures are stable across compatible versions. A later minor
may append fields whose absence has defined defaults. Check the capability
bits for features used by the host.

## Records and caller storage

Initialize the `size` and `abi` fields before passing an extensible record. The
size is the capacity in bytes, including any tail beyond the v2 prefix.
Configuration, controls, telemetry and diagnostics use prefixes of 40, 40,
136 and 2,888 bytes respectively. Each call ignores an unknown input tail and preserves
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
handle state. `ES_CONFIG`, `ES_RESOURCE` and `ES_MODEL` identify failures in
document validation, input resources and model preparation. `ES_RENDER_FAULT`
identifies a rendering fault. `ES_NO_MEMORY` and `ES_INTERNAL` cover allocation
failure and unexpected library errors.

A caller may pass an optional 512-byte `es_error`, including before a handle
exists. Each call that accepts an error record clears the message on success
or writes a bounded, NUL-terminated UTF-8 diagnostic on failure. Retain a
diagnostic by using different error storage for subsequent calls.

Load and detailed reset also accept a versioned caller-owned
`es_diagnostic` record. Initialize `size` and `abi` before calling. An
undersized record or unsupported ABI rejects the operation and leaves the detail
record untouched. Success clears the known detail fields and sets stage to
`ES_DIAG_NONE`. Failure returns the first error, a stable code, a JSON Pointer
and the source path when available. A byte offset is zero-based. Line and
Unicode-scalar column are one-based. Unavailable positions are
`ES_DIAG_NO_LOCATION`. Strings are NUL-terminated UTF-8. The `truncated`
bits identify fields that lost content at a UTF-8 character boundary.

The signed `es_status` values are OK=0, INVALID=1, VERSION=2, UNSUPPORTED=3,
STATE=4, RESOURCE=6, MODEL=7, CAPACITY=8, NO_MEMORY=9, RENDER_FAULT=10,
INTERNAL=11 and CONFIG=12. Value 5 is reserved. Check against `ES_OK`, not a
negative-error convention. Exceptions stay inside the library. Allocation
failure and unexpected exceptions during rendering still obey the completed
prefix rule. A diagnostic may be truncated at a UTF-8 boundary; it is not a
process-global last error.

## Configuration and controls

Configuration is fixed for the handle's lifetime. Initialize it with
`es_config_init(ES_ABI_V2, sizeof config, &config, &error)` before editing fields.
No packing pragma or foreign allocation ownership is part of this ABI.

| Configuration field | Default | Accepted value |
| --- | --- | --- |
| `output_hz` | 44100 | 44100 or 48000 Hz, mono float32 |
| `mode` | `ES_FREE` (0) | FREE or `ES_DRIVEN` (1) |
| `seed` | 1 | Every unsigned 64-bit value |
| `simulation_hz` | 0 | Model's rate, or integer override 4000–96000 Hz |
| `flags` | 0 | Zero; unknown bits return `ES_UNSUPPORTED` |
| `starter_torque_nm` | 0 | Inherit the model, or finite (0,10000] N·m in FREE; zero only in DRIVEN |

A model's selected numerical rate must also be a finite integer in
4000–96000 Hz unless a supported creation override supplies the rate. Changing
configuration requires a fresh handle. The starter keeps the model's signed
speed and rotation direction; a torque override changes its ceiling only.

All control defaults are zero. Set `size` and `abi`, then supply the entire
vector; an invalid field rejects the vector without changing pending controls.
Finite numeric values are required, including when another field disables an
operation. Demand has no runtime smoothing.

| Control field | Range and physical meaning |
| --- | --- |
| `ignition` | 0 or 1; enables spark, not pause or guaranteed silence |
| `starter` | 0 or 1 in FREE; uses model torque/speed; DRIVEN requires zero |
| `demand` | [0,1]; model driver demand through its direct linkage or governor |
| `load_nm` | [0,10000] N·m; independent output-crank load, described below |
| `driven_rpm` | [0,`driven_limit_rpm`] clockwise in DRIVEN; FREE requires zero |

Nonfinite values, invalid booleans and out-of-range numbers return `ES_INVALID`;
recognized but incompatible mode/control combinations return `ES_UNSUPPORTED`.
Controls latch at the next eligible tick start, persist until replaced and are
reset to defaults by successful load/reset. For simultaneous setters the last
valid complete vector wins. Split rendering at each absolute event frame.

## Calls and states

The header gives the exact cdecl signatures. All status-returning calls accept
optional caller-owned `es_error *`; queries and destruction do not need one.

| Function | Result and storage contract |
| --- | --- |
| `es_api_version` | Immutable packed major/minor, currently `ES_ABI_V2` |
| `es_capabilities` | Independent feature bits for FREE, both rates, LOAD, DRIVEN, RESET parallel prepared rendering, file and byte JSON loads, and structured diagnostics |
| `es_config_init` | Write the known default prefix only on success; retain supplied compatible ABI and capacity |
| `es_create` | Validate configuration; clear a valid output slot on entry, then return an EMPTY handle on success |
| `es_load_json_file` | Load one JSON document from an absolute UTF-8 entry with an explicit absolute asset directory |
| `es_load_json_bytes` | Copy one length-delimited document from caller storage and load using the explicit asset directory |
| `es_set_controls` | Accept or reject one whole requested vector |
| `es_render` | Zero required `written` on entry; fill at most the requested/capacity extent; return only completed frames |
| `es_get_telemetry` | Copy a coherent snapshot without advancing; preserve caller size/ABI/tail and leave the record unchanged on error |
| `es_reset` | Reprepare the original input identity/configuration/seed and replace only on success |
| `es_reset_detailed` | Perform the same reset with optional structured diagnostics |
| `es_unload` | Discard without filesystem reads; idempotent |
| `es_destroy` | Release through the library; NULL accepted; no call may subsequently use the handle |

| State | Permitted instance calls | Transitions |
| --- | --- | --- |
| `ES_EMPTY` (0) | telemetry, load, unload, destroy | Load → READY; reset/control/render return `ES_STATE` |
| `ES_READY` (1) | All calls | Successful load/reset → fresh READY; unload → EMPTY; computation fault → FAULTED |
| `ES_FAULTED` (2) | telemetry, load, reset, unload, destroy | Successful load/reset → READY; unload → EMPTY; render/control return `ES_STATE` |

Rendering successfully keeps the state READY. There is no separate running
state. Preflight failures and failed preparation preserve the current state.
Callers supply valid aligned, accessible, nonoverlapping pointers; the API does
not validate arbitrary invalid memory or races. Record headers must themselves
be accessible even when reporting a short record. Zero-frame rendering may
use a NULL PCM pointer in READY and advances nothing; a faulted handle remains
faulted. A required output slot must not already own another handle.

## Telemetry units and observation time

Initialize telemetry `size` and `abi` before calling `es_get_telemetry`. The
snapshot never reads a device or advances the source. `reserved` is zero.

| Fields | Meaning |
| --- | --- |
| `state`, `mode`, `output_hz`, `simulation_hz` | Lifecycle, immutable mode/output rate and prepared numerical rate |
| `frames`, `ticks`, `native_frames` | Completed host intervals, completed physics ticks and generated native samples |
| `cylinders`, `displacement_m3` | Loaded cylinder count and total displacement in cubic metres |
| `measured_rpm` | Magnitude: `abs(crank_rad_s) * 60 / (2*pi)` |
| `crank_rad_s` | Signed output-crank angular velocity, counterclockwise positive; forward driven rotation is negative |
| `cycle_rad` | Model output-crank phase in [0,4*pi), relative to its TDC |
| `plate_angle_rad` | Actual first-intake plate angle in radians, separate from demand |
| `requested_load_nm` | Latest accepted load request, including a request awaiting a tick |
| `load_reaction_nm`, `drive_reaction_nm` | Actual last completed tick reactions, counterclockwise positive N·m |
| `applied_driven_rpm`, `driven_limit_rpm` | Last completed tick's target and validated loaded-model command ceiling; zero in FREE |

Before the first tick, reactions and applied drive target are zero and angles
reflect the prepared model. EMPTY retains configured mode/output rate but zeros
simulation/model/control fields and counters. FAULTED exposes the last completed
frame's snapshot with the fault state, even if internal computation partially
advanced before failing. An accepted load request is visible immediately;
the corresponding actual reaction can still describe the preceding tick.

## Reset and independent instance lifetimes

The C interface in `engine_sim/runtime.h` uses library-owned opaque handles and caller-owned records, PCM and error storage. Query `es_capabilities()` for the features provided by a binary. A version number describes the ABI; it does not replace feature negotiation.

File loading retains the canonical entry and asset-root identities, the exact
JSON bytes and the full bytes of each consumed impulse. Byte loading copies the
document and has no source entry path. The API reads one complete format-v1
document of at most 16 MiB. Asset locators resolve beneath the explicit
directory. Relative API paths are invalid. Replace a model through a fresh
load. Failed preparation preserves the engine, time, pending controls and
fault status.

`es_reset` and `es_reset_detailed` rebuild from the saved recipe, original
configuration and seed. File reset checks the entry's canonical path and full
bytes. Both recipes verify the asset root, locator mappings and recorded
resource bytes. A missing, changed or unexpectedly resolved input returns
`ES_RESOURCE`, including a modified file whose new bytes are malformed. A
byte-identical rewrite at the same canonical path is equivalent. A byte-loaded
model remains independent of any former file holding the document.

A successful reset reproduces a fresh load on the same binary and floating-point environment: prepared phase, filters, random streams, zero counters and default controls. Reset discards the former timeline and returns a faulted engine to READY. An EMPTY handle has no recipe to reset and returns `ES_STATE`. Unload releases the prepared session and recipe without filesystem reads; repeated unload and destroying NULL are valid.

Serialize every operation on one handle, including telemetry, reset and destruction. Transfer an instance between threads only at a synchronized quiescent boundary. When `ES_CAP_PARALLEL_RENDER` is present, distinct handles can render concurrently while other independent handles are prepared or destroyed. Rendering has no process-wide render lock. Never race calls on one handle or reuse a handle after destruction.

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
