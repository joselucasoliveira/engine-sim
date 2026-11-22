# Contributing

Work from a fresh source checkout of this fork with the pinned submodules. Keep changes focused on the behavior you intend to modify. Preserve attribution and include the original notices when incorporating third-party files. Use the license in the relevant component directory when a dependency carries its own license.

Start with [BUILDING](BUILDING.md), run the affected checks in [TESTING](TESTING.md), and describe your result within the [platform evidence limits](docs/platforms.md).

## Reproduce a problem

Identify the revision with `git rev-parse HEAD` and list dependencies with `git submodule status --recursive`. Record the OS and architecture, compiler, CMake version, configuration and exact build/test commands. For a desktop problem include the preset, controls, observed output, expected output, window/display backend and audio output device. Include the first relevant diagnostic and exit status. Remove personal paths and credentials from shared logs. A measured signal difference should describe sample format, alignment and workload. Include a small redistributable input or instructions for generating it.

## Propose a change

Prepare a focused branch, add a regression that distinguishes the corrected behavior from its parent, and run the affected checks in the documented configurations. In your pull request to this fork describe the problem, the behavior change, validation, and the environments you exercised. Use the fork's Pull requests tab. Contribution channels are listed on the actual fork page. [Fork repository](https://github.com/joselucasoliveira/engine-sim)

Keep build output, PCM captures, traces, coverage, large test cases and machine-specific settings out of the source tree. A reusable small fixture should include its origin, permitted uses, hash and the property it tests. A solver or audio change should include comparable before/after evidence and explain intentional differences. Identify device, headless and native validation individually.

For a runtime issue, include static/shared selection, ABI/capabilities, output and
numerical rates, mode, seed, exact input identities and complete controls at
absolute frame indices. Keep event times fixed when comparing block partitions;
record requested/written frames, status and the last coherent telemetry. For a
desktop queue issue, separate source production, ready/reserved PCM, transport
submission, actual device consumption, drops and inserted silence. Include
pause/reload boundaries and distinguish initialization from stabilized idle.
Never use a timed offline render as a callback deadline or physical playback
claim. Follow the [integration contracts](docs/runtime-guide.md) and preserve
the native tuple and first failure with the result.

Preserve the first failing log when retrying. Keep reference captures and calibrated comparison policies immutable; explain each intended signal change. Reusable third-party material needs its exact source and applicable notice. Do not infer recording or SDK redistribution rights from the root license. See [third-party notices](THIRD_PARTY_NOTICES.md).

## Optional Discord presence

A Windows x64 build can opt in with `-DDISCORD_ENABLED=ON`. Set `ENGINE_SIM_DISCORD_APPLICATION_ID` to the numeric ID of an application you control before launching. Leaving it unset keeps presence inactive.
