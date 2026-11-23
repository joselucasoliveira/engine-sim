# Migrate an engine from script to JSON

Engine Simulator loads [expanded v1 JSON](engine-config.md) and
[authoring v2 JSON](engine-authoring.md) in the desktop, the device-free SDK
and the capture tool. The [catalog summary](engine-authoring-catalog.md)
lists the presets and the verification against their v1 reference models. Migrate a trusted `.mr` script
once with the separately built [converter](legacy-converter.md).
Keep the original script, library and lawful resources for future edits.
A converted file describes one evaluated engine and its selected vehicle,
transmission and desktop settings.

## Build the offline converter

From a checkout with the [converter prerequisites](../BUILDING.md#legacy-converter-and-combined-builds),
prepare the verified archives and pinned Git objects before disconnecting.
Set `CONVERTER_OUT` and `ARCHIVE_CACHE` to fresh output and existing cache
directories respectively, both absolute.

```sh
set -eu
git submodule update --init -- dependencies/submodules/simple-2d-constraint-solver dependencies/submodules/piranha
export CMAKE_GENERATOR=Ninja
export ES_OUT="$CONVERTER_OUT"
python3 -B tools/bootstrap.py --profile converter --out "$ES_OUT" \
  --cache "$ARCHIVE_CACHE" --offline
cmake --preset legacy-converter
cmake --build --preset legacy-converter --config Release --target engine-sim-convert
export CONVERTER="$ES_OUT/legacy-converter/bin/engine-sim-convert"
"$CONVERTER" --help
```

Use the matching `.exe` invocation on Windows. The converter acquires the
legacy interpreter and host parser and filesystem tools. The SDK and
desktop use their own build profiles. See [all build profiles](../BUILDING.md).
The Windows application retains the Delta adapter for rendering and audio.
Linux and macOS use SDL; the private C SDK implementation is device-free.
Native platform and deployment statuses are in the [platform matrix](platforms.md).

## Convert and validate a known engine

Use the synthetic fixture first. Run from the repository root with
`CONVERTER` from the build above and set `MIGRATION_OUT` to a new absolute
directory. The converter publishes the canonical file and a separate
invocation record.

```sh
export SCRIPT_FIXTURE="$PWD/examples/runtime/fixture"
mkdir -p "$MIGRATION_OUT"
"$CONVERTER" --entry "$SCRIPT_FIXTURE/main.mr" --library-root "$SCRIPT_FIXTURE/library" \
  --asset-root "$SCRIPT_FIXTURE" --resource-root "$SCRIPT_FIXTURE" \
  --output "$MIGRATION_OUT/engine.json" --manifest "$MIGRATION_OUT/converted.run.json"
"$CONVERTER" --entry "$SCRIPT_FIXTURE/main.mr" --library-root "$SCRIPT_FIXTURE/library" \
  --asset-root "$SCRIPT_FIXTURE" --resource-root "$SCRIPT_FIXTURE" \
  --validate-only --manifest "$MIGRATION_OUT/validated.run.json"
cmp "$MIGRATION_OUT/engine.json" "$SCRIPT_FIXTURE/main.json"
```

For your engine, set the entry and its legacy library/asset search roots.
Use a resource root containing all referenced impulses. The converter
resolves inputs in their original declaring units and rebases impulse
locators under that root. Preserve the relative layout when deploying the
JSON and WAVE files. For the app, pass the absolute JSON to `--model` and
the resource directory to `--resources`. For an installed SDK, use the same
directory as the load's asset root. For a quick check, run the
[offline renderer](runtime-render.md) into a fresh output file.

The default `entry` selection executes the script as provided. Choose
`--selector main` for a public `main` or `--selector factory:NAME` for a
public engine factory. Both can accept repeated `--arg NAME=JSON_SCALAR`
for declared formals; use JSON quotes for a string. Each invocation captures
one concrete selection. Runtime JSON contains the resulting numbers, curves
and references. To vary a factory parameter, convert each desired setting
again or edit its corresponding JSON value. Use a trusted wrapper entry
for more involved legacy compositions.

Shipped concrete model files sit beside their source presets under
[assets/engines](../assets/engines), with the application default at
[assets/main.json](../assets/main.json). The desktop theme lives in the
model's `desktop` object. To use an evaluated [theme fragment](../assets/themes),
copy its complete object into that property and load the resulting model.

## Recover from conversion or reload errors

Inspect the failing stage, input path and code in the conversion record.
Correct an unresolved import in its library tree. For a resource outside
the chosen root, prepare a lawful tree containing the expected relative
layout and repeat the conversion with that root. An invalid invocation or
model leaves a previous output available. Use a new destination for each
attempt, or explicitly select `--force` to replace an existing destination.
The [converter reference](legacy-converter.md) documents exits, recovery
and ordered batch conversion with per-item results.

When loading an edited JSON or using a relocated resource tree, call
`es_load_json_file` or `es_load_json_bytes` again. A failed load leaves the
current session available. If `es_reset_detailed` returns `ES_RESOURCE` for
a changed file or impulse, inspect its diagnostic, then restore the exact
original input and retry reset, or explicitly load the edited recipe to
start it at frame zero. Reset recalculates preparation with the original
seed and settings. The [ABI reference](runtime-abi.md#reset-and-independent-instance-lifetimes)
defines the preserved state and the [integration guide](runtime-guide.md)
shows C, C++ and dynamic-library consumers.

## Redistribute only cleared inputs

Keep copyright and license notices with code and every asset you distribute.
The code-only RuntimeSDK includes its required notices, headers and schema.
Its opt-in example uses the MIT-licensed synthetic engine and impulse.
Original recordings retain separate redistribution requirements. A local
conversion record or successful render describes the files exercised.
See the [third-party notices](../THIRD_PARTY_NOTICES.md).
