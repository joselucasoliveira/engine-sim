## Engines from *Simulating VTEC, a radial-5 engine and more with my engine simulator (there is sound)*

This folder contains all the engines from [*Simulating VTEC, a radial-5 engine and more with my engine simulator (there is sound)*](https://youtu.be/WPCMNnM6D0k). Each file contains a node called `main` and it can be used in `main.mr` as follows.

```
import "engines/atg-video-1/<insert filename>"

main()
```

The following engines are included:
1. Honda 1-cylinder ATV engine
2. Kohler Command Pro CH750
3. Harley Davidson Shovelhead
4. Hayabusa Inline-4
5. Honda B18C5
6. Subaru EJ25 (version 1)
7. Audi Inline-5
8. Radial-5

Each numbered engine also has a canonical JSON preset beside its script.
The conversion evaluates that engine's `main()` selection with the supplied
script library and retains its vehicle, transmission and impulse references.
When loading a preset, use the repository's common data root containing
`assets/` and `es/` as the asset root. [Preset index](presets.json)

The presets derive from the corresponding model scripts under the project's
MIT terms. The JSON files reference the existing sound library. The recording
files retain their own rights and distribution requirements.
