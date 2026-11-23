# Canonical Config and JSON example

This source-tree example builds against the C++17 Config and Json targets.
It converts a v1 document into the normalized engine model and writes the
same model with explicit defaults and canonical JSON formatting. It does
not open engine resources.

From the source checkout:

```sh
cmake -S examples/config -B out/config-example
cmake --build out/config-example --config Release
out/config-example/engine-sim-config-roundtrip test/config/fixtures/schema-engine.json canonical.json
```

Use the executable under `Release` with a multi-config generator.
