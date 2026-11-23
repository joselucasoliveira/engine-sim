# Experimental Fleet preparation

The build-tree C11 and C++17 interface in `engine_sim/fleet.h` is available through
`EngineSim::Fleet` when the device-free runtime is enabled. This increment supports
owner creation, detached voice preparation, discard and destruction.

Initialize each extensible input with its size and `ES_RT_V1`. For local
experiments, create an owner with `ES_RT_EXPERIMENTAL` and qualification ID zero.
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
its last use. For optional diagnostics, initialize `es_rt_diagnostic.size` and
`es_rt_diagnostic.abi`. The call writes a bounded UTF-8 message and numeric stage
and status. Success clears the diagnostic.
