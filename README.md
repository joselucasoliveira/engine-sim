# Engine Simulator

This independently maintained fork of Ange Yaghi's Engine Simulator models internal combustion engines in real time. You can load an engine description, operate its starter and controls, and hear the simulated exhaust. The model is intended for exploring engine response and sound. Its predictions are unsuitable for engineering validation or tuning decisions. [Original project](https://github.com/ange-yaghi/engine-sim)

## Getting the source

Clone this fork, including its pinned dependencies. Run this command in a terminal in the parent directory where you want the checkout.

```text
git clone --recurse-submodules https://github.com/joselucasoliveira/engine-sim.git
```

The inherited application starts through a Windows desktop entry point. Native Windows, Linux and macOS support for this fork is under validation. A platform claim requires a tested configuration and feature list. Source builds are the delivery path described here.

## Driving the simulator

The supplied entry script selects a Subaru EJ25 equal-length-header example and the default theme. The engine definitions in the two compilation-video folders expose a node named `main`. To select another one in a writable configuration, edit the engine import in `assets/main.mr`, retaining the engine library import, the theme call and the call to `main()`. [Entry script](assets/main.mr) · [First preset collection](assets/engines/atg-video-1/README.md) · [Second preset collection](assets/engines/atg-video-2/README.md)

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
| Enter / Escape | Reload the script / exit |

Engage the dynamometer with D after enabling hold with H. Read the target on the DYNO. SPEED gauge. Some settings depend on the engine preset and the operating state.

## Credit and contributions

Ange Yaghi created the original simulator. The original copyright and license accompany this fork. See the contribution guide for reproducing a problem and proposing a change. [License](LICENSE) · [Contributing](CONTRIBUTING.md)
