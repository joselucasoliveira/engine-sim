# Catalog authoring migration

The catalog has 27 authoring models with all 239 selected curve functions
mapped. Each model uses its original JSON path. The converter regenerated the
frozen v1 data from the corresponding .mr and its selected factory. The
compiled v2 expansion matched the complete v1 byte stream for every preset,
including binary64 values, signed zero, sample order, shared references,
ignition phases and WAVE locators. The same Linux x86-64 GCC 14.2 Release
renderer produced identical PCM and final RPM in 91 cases. Each used the
same seed, resource root, frame count and control schedule. The deep cases
cover FREE startup and operation plus DRIVEN at 44.1 and 48 kHz.

## Authoring inputs by curve type

| Type | Source inputs and defaults | Effective values |
| --- | --- | --- |
| 46 harmonic cam profiles | The [script helper](../es/actions/actions.mr#L202) takes duration at 50 thou, lift, gamma and steps. The lift unit follows the original source. Lift defaults to 300 thou, gamma to 1 and steps to 100. | Degree and lift conversion, radius, center sample, ordered mirrored samples and zero-lift padding. |
| 111 measured port functions | Source lift and SCFM stay as points. The [GM LS head](../assets/engines/atg-video-2/07_gm_ls.mr#L19) has independent lift and flow overrides. The [BMW head](../assets/engines/bmw/M52B28.mr#L108) uses millimetres. Radial heads share the original helper's scale. | Apply shared scale references, convert lift units and evaluate the 28-inch-of-water coefficient in the legacy order. |
| 27 ignition curves | Original RPM, signed crank advance in degrees and radius in RPM. | Convert units, retaining every point in insertion order. |
| 27 flame-speed curves | Turbulence samples, flame speed or the original multiplier and row scale, and the original radius. | Evaluate the left-to-right products. The points stay explicit, including floors and per-row overrides. |
| 1 measured cam profile | Cam angle in degrees, lift in metres and filter radius in degrees from the [MIT fixture](../examples/runtime/fixture/main.mr#L25). | Convert angles, preserving both camshafts' shared profile and all literal points. |
| 27 default piston-speed curves | The legacy engine provides this implicit table and has no factory input for it. | Generate the original 30 points and radius in code. |

The v2 authoring work covers the catalog's curve functions. Other model
fields use the effective component records, including
[geometry and deck height](../assets/engines/atg-video-2/07_gm_ls.mr#L263),
[camshaft centerlines](../assets/engines/atg-video-2/07_gm_ls.mr#L75) and
the drivetrain.

## Per-preset results

The catalog went from 957,759 expanded bytes to 335,719 authored bytes.
No preset required the v1 fallback. The v1 references and hashes are kept
in migration evidence, and the catalog indexes record their hashes.
The lobe tuples give duration in degrees, lift in the stated unit, gamma
and steps, in function-table order. The measured port columns count
tables and scale records.

| Preset | Harmonic authoring tuples | Curves | Port tables / scales | Bytes v1 → v2 | Effective model / PCM cases |
| --- | --- | ---: | ---: | ---: | --- |
| [atg-video-1/01_honda_trx520](../assets/engines/atg-video-1/01_honda_trx520.json) | (180, 200 thou, 1.0, 100) | 6 | 2 / 1 | 17,439 → 7,668 | Pass / 1 |
| [atg-video-1/02_kohler_ch750](../assets/engines/atg-video-1/02_kohler_ch750.json) | (160, 200 thou, 1.1, 100) | 8 | 2 / 2 | 20,527 → 9,618 | Pass / 5 |
| [atg-video-1/03_harley_davidson_shovelhead](../assets/engines/atg-video-1/03_harley_davidson_shovelhead.json) | (210, 400 thou, 0.9, 100) | 8 | 2 / 2 | 20,691 → 9,928 | Pass / 1 |
| [atg-video-1/04_hayabusa](../assets/engines/atg-video-1/04_hayabusa.json) | (240, 345 thou, 1.2, 100) / (220, 294 thou, 1.2, 100) | 7 | 2 / 1 | 29,600 → 9,972 | Pass / 1 |
| [atg-video-1/05_honda_vtec](../assets/engines/atg-video-1/05_honda_vtec.json) | (210, 6.9 mm, 1.0, 100) / (190, 6.5 mm, 1.0, 100) / (240, 11.5 mm, 0.5, 100) / (232, 10.5 mm, 0.5, 100) | 9 | 2 / 1 | 50,204 → 10,841 | Pass / 5 |
| [atg-video-1/06_subaru_ej25](../assets/engines/atg-video-1/06_subaru_ej25.json) | (232, 9.78 mm, 2.0, 100) / (236, 9.6 mm, 2.0, 100) | 9 | 2 / 2 | 31,511 → 11,194 | Pass / 5 |
| [atg-video-1/07_audi_i5](../assets/engines/atg-video-1/07_audi_i5.json) | (210, 9.78 mm, 2.0, 100) / (215, 9.6 mm, 2.0, 100) | 7 | 2 / 1 | 30,499 → 10,881 | Pass / 1 |
| [atg-video-1/08_radial_5](../assets/engines/atg-video-1/08_radial_5.json) | (260, 800 thou, 0.9, 100) | 14 | 2 / 1 | 29,820 → 15,921 | Pass / 5 |
| [atg-video-2/01_subaru_ej25_eh](../assets/engines/atg-video-2/01_subaru_ej25_eh.json) | (232, 9.78 mm, 2.0, 100) / (236, 9.6 mm, 2.0, 100) | 9 | 2 / 2 | 31,242 → 11,023 | Pass / 5 |
| [atg-video-2/02_subaru_ej25_uh](../assets/engines/atg-video-2/02_subaru_ej25_uh.json) | (232, 9.78 mm, 2.0, 100) / (236, 9.6 mm, 2.0, 100) | 9 | 2 / 2 | 31,239 → 11,020 | Pass / 5 |
| [atg-video-2/03_2jz](../assets/engines/atg-video-2/03_2jz.json) | (220, 9.78 mm, 1.1, 100) / (220, 9.6 mm, 1.1, 100) | 7 | 2 / 1 | 31,185 → 11,611 | Pass / 1 |
| [atg-video-2/04_60_degree_v6](../assets/engines/atg-video-2/04_60_degree_v6.json) | (222, 400 thou, 1.0, 100) / (226, 300 thou, 1.0, 100) | 9 | 2 / 2 | 33,059 → 12,875 | Pass / 5 |
| [atg-video-2/05_odd_fire_v6](../assets/engines/atg-video-2/05_odd_fire_v6.json) | (222, 400 thou, 1.0, 100) / (226, 300 thou, 1.0, 100) | 9 | 2 / 2 | 32,855 → 12,686 | Pass / 5 |
| [atg-video-2/06_even_fire_v6](../assets/engines/atg-video-2/06_even_fire_v6.json) | (222, 400 thou, 1.0, 100) / (226, 300 thou, 1.0, 100) | 9 | 2 / 2 | 33,072 → 12,930 | Pass / 5 |
| [atg-video-2/07_gm_ls](../assets/engines/atg-video-2/07_gm_ls.json) | (234, 551 thou, 1.1, 256) / (235, 551 thou, 1.1, 256) | 9 | 2 / 2 | 65,803 → 13,858 | Pass / 5 |
| [atg-video-2/08_ferrari_f136_v8](../assets/engines/atg-video-2/08_ferrari_f136_v8.json) | (230, 551 thou, 0.9, 256) / (230, 551 thou, 0.9, 256) | 9 | 2 / 2 | 65,664 → 13,805 | Pass / 5 |
| [atg-video-2/09_radial_9](../assets/engines/atg-video-2/09_radial_9.json) | (260, 800 thou, 0.9, 100) | 22 | 2 / 1 | 42,118 → 23,971 | Pass / 5 |
| [atg-video-2/10_lfa_v10](../assets/engines/atg-video-2/10_lfa_v10.json) | (230, 15.95 mm, 1.1, 100) / (230, 15.95 mm, 1.1, 100) | 9 | 2 / 2 | 35,183 → 15,160 | Pass / 1 |
| [atg-video-2/11_merlin_v12](../assets/engines/atg-video-2/11_merlin_v12.json) | (242, 15.95 mm, 0.8, 100) / (246, 590 thou, 0.8, 100) | 9 | 2 / 2 | 36,647 → 16,692 | Pass / 5 |
| [atg-video-2/12_ferrari_412_t2](../assets/engines/atg-video-2/12_ferrari_412_t2.json) | (242, 15.95 mm, 0.8, 512) / (246, 15.95 mm, 0.8, 512) | 9 | 2 / 2 | 120,181 → 16,655 | Pass / 5 |
| [audi/i5](../assets/engines/audi/i5.json) | (232, 578 thou, 0.75, 100) | 6 | 2 / 1 | 20,650 → 11,020 | Pass / 1 |
| [bmw/M52B28](../assets/engines/bmw/M52B28.json) | (210, 9 mm, 0.8, 100) / (210, 9 mm, 0.8, 100) | 7 | 2 / 1 | 31,116 → 11,548 | Pass / 5 |
| [chevrolet/chev_truck_454](../assets/engines/chevrolet/chev_truck_454.json) | (194, 390 thou, 0.8, 100) / (202, 409 thou, 0.8, 100) | 9 | 2 / 0 | 34,917 → 14,119 | Pass / 1 |
| [chevrolet/engine_03_for_e1](../assets/engines/chevrolet/engine_03_for_e1.json) | (232, 578 thou, 0.75, 100) | 8 | 2 / 0 | 24,954 → 14,123 | Pass / 1 |
| [kohler/kohler_ch750](../assets/engines/kohler/kohler_ch750.json) | (160, 200 thou, 1.1, 100) | 8 | 2 / 2 | 20,443 → 9,535 | Pass / 1 |
| [Desktop default](../assets/main.json) | (232, 9.78 mm, 2.0, 100) / (236, 9.6 mm, 2.0, 100) | 9 | 2 / 2 | 31,242 → 11,023 | Pass / 5 |
| [MIT fixture](../examples/runtime/fixture/main.json) | — | 5 | 1 / 0 | 5,898 → 6,042 | Pass / 1 |

Use the [catalog driver](../test/config/authoring_catalog.py) with the
Release converter, writer and renderer to reproduce the migration.
The migrate action takes a directory with the original v1 JSON files
under their catalog paths. It writes candidates and per-function origins
to a new external directory. The verify action checks the shipped JSON
paths against the pinned expansion hashes. It can also compare directly
with a frozen v1 directory when supplied. Both exercise PCM and RPM
parity. Native Windows and macOS parity require their own runs.
