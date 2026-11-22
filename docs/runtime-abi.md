# Runtime C ABI version 1

The preview [runtime header](../include/engine_sim/runtime.h) declares the
version 1 contract for C11 and C++17 hosts. This checkout provides declaration
and layout checks. The preview header has no install or link target.

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
