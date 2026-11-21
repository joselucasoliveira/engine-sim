# Contributing

Work from a fresh source checkout of this fork with the pinned submodules. Keep changes focused on the behavior you intend to modify. Preserve attribution and include the original notices when incorporating third-party files. Use the license in the relevant component directory when a dependency carries its own license.

## Reproduce a problem

Identify the revision with `git rev-parse HEAD` and list dependencies with `git submodule status --recursive`. Record the OS and architecture, compiler, CMake version, configuration and exact build/test commands. For a desktop problem include the preset, controls, observed output, expected output, window/display backend and audio output device. Include the first relevant diagnostic and exit status. Remove personal paths and credentials from shared logs. A measured signal difference should describe sample format, alignment and workload. Include a small redistributable input or instructions for generating it.

## Propose a change

Prepare a focused branch, add a regression that distinguishes the corrected behavior from its parent, and run the affected checks in the documented configurations. In your pull request to this fork describe the problem, the behavior change, validation, and the environments you exercised. Use the fork's Pull requests tab. Contribution channels are listed on the actual fork page. [Fork repository](https://github.com/joselucasoliveira/engine-sim)

Keep build output, PCM captures, traces, coverage, large test cases and machine-specific settings out of the source tree. A reusable small fixture should include its origin, permitted uses, hash and the property it tests. A solver or audio change should include comparable before/after evidence and explain intentional differences. Identify device, headless and native validation individually.

## Optional Discord presence

A Windows x64 build can opt in with `-DDISCORD_ENABLED=ON`. Set `ENGINE_SIM_DISCORD_APPLICATION_ID` to the numeric ID of an application you control before launching. Leaving it unset keeps presence inactive.
