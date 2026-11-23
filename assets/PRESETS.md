# Engine presets and themes

The [catalog](preset-catalog.json) lists the engine preset indexes, the
licensed runtime fixture and the available desktop themes. [Historical presets](presets-historical.json)

Use the common data directory containing `assets/` and `es/` as the asset
root for these engine presets. Each historical JSON file preserves its own
named factory selection beside the source script. These model files derive
from the corresponding source files under the project's MIT terms. Recorded
impulse responses remain separate resources and retain their own
distribution requirements.

[The default selection](main.json) evaluates the equal-header EJ25 and the
default theme. It carries the EJ25 vehicle and transmission. Its boost
display uses `PSI`, the spelling set by the default theme. The [standalone
equal-header EJ25](engines/atg-video-2/01_subaru_ej25_eh.json) uses `psi`.

A theme file contains a complete desktop settings object. Copy the object
into an engine model's `desktop` field before loading the model to apply the
theme. The loaded model owns the selected settings.
