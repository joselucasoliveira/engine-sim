# Offline engine fixture

This original single-cylinder numerical fixture uses an 80 mm bore, 80 mm
stroke and 40 cc clearance volume. Its nominal displacement is
0.0004021238596594936 cubic metres. It runs at 10,000 simulation ticks per
second. It provides a direct throttle linkage and leaves vehicle and
transmission selection to the host's default preparation.

The engine script and synthetic impulse generator are provided under the
project's MIT license. The small script library retains Ange Yaghi's copyright
and the accompanying MIT license. Trailing whitespace is removed from the copies. Source paths and both source
and copied SHA-256 values are recorded in `library/UPSTREAM.json`.

Regenerate `impulse.wav` with Python 3 from any working directory.

```sh
python3 /absolute/fixture/generate_impulse.py /absolute/external/impulse.wav
```

The response consists of eight explicitly specified PCM16 samples at 44.1 kHz.
Supply this directory as the asset root, `main.mr` as the entry and `library`
as the script library root. The library closure contains only the script
types, units, operations, actions and model objects used to prepare engines.
