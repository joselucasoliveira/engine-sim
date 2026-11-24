# Engine Simulator

This independently maintained fork of Ange Yaghi's Engine Simulator models internal combustion engines in real time. You can load an engine description, operate its starter and controls, and hear the simulated exhaust. The model is intended for exploring engine response and sound. Its predictions are unsuitable for engineering validation or tuning decisions. [Original project](https://github.com/ange-yaghi/engine-sim)

## Getting the source

Clone this fork, including its pinned dependencies. Run this command in a terminal in the parent directory where you want the checkout.

```text
git clone --recurse-submodules https://github.com/joselucasoliveira/engine-sim.git
```

The Linux desktop runs the existing engine views and controls through SDL 3, Vulkan and SDL audio. The Windows adapter retains Win32, Direct3D and DirectSound. The macOS Metal/CoreAudio path is implemented but awaits a native run. Read the [build instructions](BUILDING.md), [tests](TESTING.md) and [feature-by-feature platform status](docs/platforms.md). Source builds and explicitly private validation installs are the current delivery path.

## Embed or render without a desktop

The C runtime SDK loads engine models from JSON and renders caller-owned mono
float PCM at 44.1 or 48 kHz without a window, device or worker. It supports
free-running engines, an independent shaft load and solver-driven clockwise
rotation. Static/shared C11, C++17 and ctypes examples accompany the
[integration guide](docs/runtime-guide.md), [engine JSON format](docs/engine-config.md),
[ABI reference](docs/runtime-abi.md)
and [offline WAV renderer](docs/runtime-render.md). The optional installed
example uses a synthetic impulse; code-only SDKs include no original recordings.
Linux installed execution is exercised locally. Native Windows/macOS and hosted
SDK jobs remain pending; callback deadlines are unqualified.

For multiple independent vehicle voices, see the
[Fleet SDK integration guide](docs/fleet-guide.md). Fleet 1.0 provides
device-neutral C11 and C++17 examples with a shared mix and separate stems.

## Driving the simulator

The default model is a Subaru EJ25 with equal-length headers and the default
theme. Pass `--model ABSOLUTE_JSON_PATH` to select another engine. For example,
`assets/engines/atg-video-2/09_radial_9.json` selects the nine-cylinder radial.
Pass `--resources ABSOLUTE_DIRECTORY` when using a different asset root.
Keep the installed resource tree intact so its model and impulse paths retain
their verified identities. Convert existing scripts with the offline
converter. [Default model](assets/main.json) · [Engine migration guide](docs/engine-migration.md)

| Input | Action |
| --- | --- |
| A / hold S | Toggle ignition / engage the starter |
| Q, W, E, R | Set increasingly large throttle openings while held |
| Space + wheel | Adjust throttle finely |
| D / H / G + wheel | Toggle the dynamometer / toggle speed hold / adjust its target while hold is enabled |
| Up / Down / Shift | Shift up / down / depress the clutch. Hold Space for slower clutch response |
| Z / X / C / V / B + wheel | Adjust volume / convolution / high-frequency gain / airflow noise / input noise |
| N + wheel | Change simulation frequency |
| M / comma | Show deeper / shallower engine layers |
| 1–5 | Slow the simulation while held |
| F / Tab | Toggle fullscreen / cycle display layouts |
| Enter / Escape | Reload the model / exit |

Engage the dynamometer with D after enabling hold with H. Read the target on the DYNO. SPEED gauge. Some settings depend on the engine preset and the operating state.

## Credit and contributions

Ange Yaghi created the original simulator. The original copyright and license accompany this fork. See the contribution guide for reproducing a problem and proposing a change. [License](LICENSE) · [Third-party notices](THIRD_PARTY_NOTICES.md) · [Contributing](CONTRIBUTING.md)

The original impulse recordings retain their upstream provenance. Separate recording redistribution clearance is unresolved; the default installer blocks a distributable stage. See [private staging and resource layout](BUILDING.md). Physical playback, listening and complete native Windows/macOS qualification remain pending.
