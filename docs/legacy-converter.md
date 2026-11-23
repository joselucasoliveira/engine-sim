# Offline legacy engine converter

The converter evaluates trusted Engine Sim scripts and exports v1 JSON plus
a private source and resource manifest. It is an optional local build target.
The runtime does not invoke it.

Build the converter from the project checkout with an external build directory
and a populated locked archive cache.

```sh
export ES_OUT=/absolute/path/to/converter-build
python3 tools/bootstrap.py --profile converter --out "$ES_OUT" --cache /absolute/path/to/archive-cache --offline
cmake --preset legacy-converter
cmake --build --preset legacy-converter --config Release --target engine-sim-convert
```

For a script module with a public `main`, use absolute paths.

```sh
engine-sim-convert --entry /project/engine-sim/assets/engines/atg-video-1/05_honda_vtec.mr \
  --library-root /project/engine-sim/es --asset-root /project/engine-sim/assets \
  --resource-root /project/engine-sim --selector main \
  --output /project/runs/05_honda_vtec.json --manifest /project/runs/05_honda_vtec.run.json
```

Use `--selector entry` (the default) to execute the original file, or
`--selector factory:IDENT` to select a public engine factory. The two named
selections accept repeated `--arg NAME=JSON_SCALAR` for their declared
inputs. The converter checks string and number literals against the
representable legacy forms. Use `--validate-only` in place of `--output`
to validate and hash the proposed bytes without creating an engine file.

The asset and library roots are legacy search roots. The resource root
contains the canonical WAVE files. Relative impulse names resolve from their
declaring script. The manifest tracks actual scripts and resources, output
hashes, and the stage and path of a failure. It does not establish parity
or redistribution clearance.

Use `--batch /path/to/plan.json --manifest /path/to/result.json` to run an
ordered v1 JSON plan. Each item supplies `key`, `entry`, `library_root`,
`asset_root`, `resource_root`, `selector`, and `args`, plus either
`output` or `validate_only: true`. A failing item leaves other items
eligible. After interruption, rerun the same command. The journal resumes
from verified input and output identities. A mixed result exits with 8.
Rerun an individual failure using its selector, roots and named arguments.

Existing destinations require `--force`. A destination cannot alias a
source or resource that the invocation uses. Keep original scripts, their
assets and their notices.

| Exit | Meaning |
| --- | --- |
| 0 | All items converted or validated |
| 2 | Invalid arguments, plan or selection |
| 3 | Legacy compilation or evaluation failed |
| 4 | The evaluated graph or serialized model is invalid |
| 5 | A source or impulse is unavailable, changed or invalid |
| 6 | Output, journal or destination collision |
| 7 | Internal or allocation failure |
| 8 | At least one batch item failed |
