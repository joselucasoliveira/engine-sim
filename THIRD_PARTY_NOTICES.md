# Credits and third-party notices

Ange Yaghi created the original Engine Simulator. This fork retains the original copyright and complete [MIT license](LICENSE). The inherited engine descriptions, scene and examples retain their attribution, and the preset collection readmes credit the original demonstrations. [Original project](https://github.com/ange-yaghi/engine-sim)

| Component | License and attribution | Source or accompanying notice |
| --- | --- | --- |
| Delta Studio | MIT, copyright 2020 Ange Yaghi. The Windows backend and portable widget math follow its conventions. Embedded code, fonts and SDK files have their own terms. | [Project](https://github.com/ange-yaghi/delta-studio) · [MIT license](assets/licenses/delta-studio.txt) |
| simple-2d-constraint-solver and csv-io | MIT, copyright 2022 Ange Yaghi. Each component retains its complete license. | [Solver](https://github.com/ange-yaghi/simple-2d-constraint-solver) · [csv-io](https://github.com/ange-yaghi/csv-io) |
| Piranha | MIT. Parser builds also use the independent notice in the Flex lexer header. | [Piranha](https://github.com/ange-yaghi/piranha) · Accompanying Piranha and FlexLexer notices |
| Boost | Boost Software License 1.0. The parser uses Boost.Filesystem. | The locked Boost source archive includes `LICENSE_1_0.txt` |
| SDL 3 | Zlib license. The Unix backend builds SDL 3.2.8 from the locked source archive. | [SDL](https://github.com/libsdl-org/SDL) · The archive includes `LICENSE.txt` |
| SDL GPU shaders | MIT. The shader and buffer conventions are adapted from Open Engine Simulator and Delta Studio. | [Shader licenses and exact sources](assets/shaders/sdl/LICENSE.txt) |
| stb_truetype 1.24 | MIT alternative, copyright 2017 Sean Barrett. The bundled copy retains the original Delta Studio and Open Engine Simulator rasterizer code, with one indentation correction. | [MIT notice](assets/licenses/stb_truetype-MIT.txt) · Full alternatives remain in the header |
| Silkscreen regular | Jason Kottke's original font-specific license. The font and its notice are copied without modification. | [Original license](<assets/fonts/Silkscreen/Kottke Silkscreen License.txt>) |
| Discord RPC | MIT, copyright 2017 Discord, Inc. The inherited wrapper credits SanGawku. | [RPC notice](dependencies/discord/LICENSE) |
| nlohmann/json 3.12.0 | MIT, copyright Niels Lohmann and contributors. The private single header retains the embedded attribution. | [Pinned source](https://github.com/nlohmann/json/tree/55f93686c01528224f448c19128836e7df245f72) · [MIT license](modules/json/third_party/nlohmann/LICENSE.MIT) |
| GoogleTest | BSD-style license. This is a test framework and is excluded from runtime installation. | [GoogleTest](https://github.com/google/googletest) · The locked source archive includes `LICENSE` |

The runtime asset allowlist records the origin, hash, accompanying license and distribution status of each selected file. The installation includes notices for the material it uses. Optional video and Discord integrations are excluded from the default runtime installation.

The standard impulse recordings retain their upstream provenance and original gains. Separate recording redistribution clearance has not been established. The redistribution installer enforces that gate; explicitly private staging retains the original bytes and marks the tree for local validation only.

The runtime fixture under `examples/runtime/fixture` contains an original
synthetic engine and an eight-sample generated impulse response under MIT.
Its minimal script library copies the listed `es` sources with trailing
whitespace removed.
The original copyright notices and MIT license accompany that copy, with
source paths and SHA-256 identities in `library/UPSTREAM.json`.

The C runtime SDK installs the original Engine Simulator MIT notice, the
solver MIT notice and the nlohmann JSON MIT notice. Its default code-only
component includes the engine configuration schema. The optional runtime
example adds the canonical single-cylinder JSON, its synthetic impulse
response and their original MIT notice.
