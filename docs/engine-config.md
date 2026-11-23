# Engine configuration format v1

An Engine Simulator model is one UTF-8 JSON document. Its root contains
`format_version`, `selected_engine`, `engine` and `components`. Use the
number `1` for `format_version` and the engine's ID for `selected_engine`.
Use the [complete synthetic model](../examples/runtime/fixture/main.json) as
a working starting point. The runtime and desktop use the same engine graph.
The [runtime guide](runtime-guide.md) shows how to load a file, load bytes or
construct the same values in C++.

## Schema and validation

The [schema](../schemas/engine-config-v1.schema.json) uses JSON Schema Draft
2020-12 and has the immutable ID `urn:engine-sim:config:1`. RuntimeSDK
installs it under `share/engine-sim-runtime/schema/engine-config-v1.schema.json`.
ConfigSDK with its Json component installs identical schema bytes under
`share/engine-sim-config/schema/engine-config-v1.schema.json`. These are
relative to the install prefix when the default data directory is used.
The model format, C ABI 2.0 and C++ ConfigSDK are versioned independently.

Keep objects closed and supply all required fields. The parser accepts one
document up to 16 MiB, applies bounded depth and collection limits, and rejects
duplicate keys, trailing commas, comments, BOM, nonfinite numbers and invalid
UTF-8. Each referenced ID must identify a definition of the expected type.
Choose an ID once per definition and reuse it when several components share
the same object. Keep ordered lists in their original order, especially shaft
journals, bank cylinders, port assignments, cam lobes, ignition posts and
curve samples. The schema checks structure. Model validation also checks IDs,
reference kinds, ownership, usable curves and topology. Preparation validates
the chosen runtime settings and every selected impulse.

The canonical writer emits effective defaults, sorted object keys and the
original array order. It writes round-trip binary64 values, preserves signed
zero and ends with one newline. Use the installed C++ Json component to
normalize authored JSON and serialize typed values. The schema contains only
local references.

## Units and defaults

| Input | v1 units and interpretation |
| --- | --- |
| Geometry and mass | Metres, square metres, cubic metres, kilograms and kg·m² inertia. A crank throw is a radius. |
| Rotation and angles | Declared speeds are in rad/s, and angles in radians. The public runtime drive control takes clockwise RPM. |
| Combustion | Absolute pressure in Pa, temperature in K, mass density in kg/m³, molar mass in kg/mol and heat energy in J/kg. |
| Loads and timing | Force in N, torque in N·m, frequency in Hz and time in seconds. |
| Port and runner flow | Evaluated Engine Simulator flow coefficients. |
| Impulses and sound | Relative WAVE locators and linear gains. A migrated gain retains its evaluated value. |

Curve units depend on their use. A cam curve maps cam radians to lift in
metres. A head-port curve maps lift to the evaluated flow coefficient.
Ignition timing maps signed speed to advance in crank radians. Preserve each
evaluated point when moving a script to JSON.

Omit a whole optional record to use its fallback. A present record needs all
of its required fields.

| Optional record | Fallback |
| --- | --- |
| `initialization` | `r2-piston-v1`, 101325 Pa chamber and crankcase pressure, and 298.15 K chamber temperature. |
| `vehicle` | 1597 kg, differential ratio 3.42, 0.254 m tire radius, drag coefficient 0.25, 6 ft × 6 ft frontal area and 2000 N rolling resistance. |
| `transmission` | Ratios `[2.97, 2.07, 1.43, 1, 0.84, 0.56]`, clutch rated at 1000 lb-ft in the original conversion, and neutral at start. |
| `desktop` | Windowed view, default colors, hp, lb-ft, mph, inHg and psi. |

A legacy factory may have different defaults. The converter writes the
evaluated values. Runtime-output rate, drive mode and seed are settings on
the new handle. See the [C ABI reference](runtime-abi.md#configuration-and-controls).

## Relative resources and diagnostics

Pass an absolute asset root to either runtime load function. Place its
regular WAVE files beneath that directory and write locator components with
`/` separators, for example `ir/short.wav`. Empty components, `.`, `..`,
absolute paths, drive prefixes, colons, backslashes and control characters
fail validation. A symlink that resolves outside the root fails containment.
Move a model and its resources together and load it against the new root.
Reset rechecks the exact bytes and paths consumed by the original load.

Read the diagnostic stage, ASCII code and JSON Pointer together. For example,
`CFG_REF` at `/components/banks/0/cylinders/0/rod_journal` identifies the
first cylinder's journal reference. Correct the target ID and its type.
`CFG_VERSION` means the declared format version is unsupported.
`CFG_RESOURCE_PATH`, `CFG_RESOURCE_BYTES` and `CFG_RESOURCE_FORMAT` identify
path, input-byte and WAVE problems respectively. File and byte decode can
report a zero-based byte offset and one-based line and column when available.
The [ABI status and diagnostics table](runtime-abi.md#status-and-diagnostics)
lists the numeric return contracts.
