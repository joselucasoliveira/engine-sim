# Building libengine-sim

Use CMake 3.25 or newer, Python 3.9 or newer, Git and a C++17 compiler. Linux development has been exercised with CMake 3.25.3 and 3.31.6, GCC 14.2 and Clang 19. See [platform evidence and limitations](docs/platforms.md) before treating a configuration as supported.

## Source and dependencies

```sh
git clone https://github.com/joselucasoliveira/libengine-sim.git
cd libengine-sim
git rev-parse HEAD
```

The public URL obtains published revisions. To test an unpublished local branch, use `git clone --no-local --branch BRANCH ABSOLUTE_LOCAL_REPOSITORY ABSOLUTE_NEW_CHECKOUT` and verify its actual HEAD. Initialize the source dependencies for your selected profile at the committed pins.

Presets require `ES_OUT` to name an absolute directory outside the checkout. Use a different output root for each compiler or diagnostic configuration. The bootstrap verifies archives against [the dependency lock](cmake/dependencies.lock.json), prepares dependencies and writes `deps.cmake` and `dependency-receipt.json` there. It does not install operating-system packages or initialize Git submodules. Tests acquire GoogleTest through the explicit `--tests` flag. Initialize the applicable source paths under `dependencies/submodules/` with `git submodule update --init -- <paths>`.

| Bootstrap profile | Pinned source paths | Additional inputs |
| --- | --- | --- |
| `config`, `synthesis` | None | Config with JSON uses its checked-in parser and Python schema compiler. |
| `physics`, `runtime` | `simple-2d-constraint-solver` | CMake, the native compiler and the system runtime. |
| `core`, `capture` | `simple-2d-constraint-solver` | Capture uses Python for orchestration. |
| `desktop` on Linux or macOS | `simple-2d-constraint-solver` | SDL 3.2.8 and the platform prerequisites below. |
| `desktop` on Windows x64 | `simple-2d-constraint-solver`, `delta-studio` | Boost.Filesystem 1.78.0, SDL2, SDL2_image and D3DX from the lock. |
| `converter` | `simple-2d-constraint-solver`, `piranha` | Boost.Filesystem 1.78.0, Flex 2.6.4 and Bison 3.2 or newer. Windows bootstrap supplies the locked generators. |

The solver stays at the revision in `cmake/dependencies.lock.json`. CMake verifies
the workspace allocation patch and the original and patched file hashes, then
compiles the patched copy in the build tree. Build-tree C++ Physics consumers
get the matching headers through `EngineSim::Physics`. The installed C Runtime
SDK uses the same solver code and ships its MIT notice. Its public header
requires only the C runtime interface.

## Linux x86_64

For the Linux desktop, install Vulkan and the development headers for the selected display and audio backends. On Debian or Ubuntu use the following packages.

```sh
sudo apt-get install build-essential cmake ninja-build git python3 \
  libvulkan-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
  libxfixes-dev libxss-dev libwayland-dev wayland-protocols libxkbcommon-dev \
  libdrm-dev libgbm-dev libasound2-dev libpulse-dev
```

A functioning display, Vulkan ICD and audio endpoint are required for interactive use. From the checkout, in Bash:

```sh
set -e
export CMAKE_GENERATOR=Ninja
export ES_OUT="$(cd .. && pwd)/engine-sim-output-linux"
git submodule update --init -- dependencies/submodules/simple-2d-constraint-solver
python3 -B tools/bootstrap.py --profile desktop --tests --out "$ES_OUT"
cmake --preset desktop
cmake --build --preset desktop -j 4
ctest --preset desktop --no-tests=error --output-on-failure
ENGINE_SIM_STAGE_MODE=local-validation cmake --install "$ES_OUT/desktop" \
  --config Release --prefix "$ES_OUT/stage"
(cd "$ES_OUT" && ./stage/bin/engine-sim-app)
```

Check each exit status before continuing; scripts should use `set -e`. The explicit staging mode above creates a **private local validation installation**. Redistribution clearance for the original impulse recordings remains unresolved. The default installer rejects a distribution stage before copying those assets. `-DENGINE_SIM_LOCAL_RESOURCE_STAGE=ON` is the equivalent explicit configure-time choice. Neither mode authorizes publishing a package.

## Windows x64

The native recipe is prepared; MSVC, Direct3D and DirectSound execution are still pending validation. Use x64 Developer PowerShell for Visual Studio 2022 with v143 C++ tools, Windows SDK, CMake, Python and Git. Install the MSVC AddressSanitizer component to use `win-asan`. The legacy renderer also needs its DirectX runtime imports; their deployment must be checked on the native host.

Run the following in x64 Developer PowerShell.

```powershell
$ErrorActionPreference = 'Stop'
$env:ES_OUT = Join-Path (Get-Item ..).FullName 'engine-sim-output-windows'
git submodule update --init -- dependencies/submodules/simple-2d-constraint-solver dependencies/submodules/delta-studio
if ($LASTEXITCODE) { throw 'Submodule setup failed' }
py -3 -B tools/bootstrap.py --profile desktop --tests --out $env:ES_OUT
if ($LASTEXITCODE) { throw 'Bootstrap failed' }
cmake --preset desktop-windows
if ($LASTEXITCODE) { throw 'Configure failed' }
cmake --build --preset desktop-windows --config Release
if ($LASTEXITCODE) { throw 'Build failed' }
ctest --preset desktop-windows -C Release --no-tests=error --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed' }
$env:ENGINE_SIM_STAGE_MODE = 'local-validation'
cmake --install "$env:ES_OUT/desktop-windows" --config Release --prefix "$env:ES_OUT/stage"
if ($LASTEXITCODE) { throw 'Install failed' }
& "$env:ES_OUT/stage/bin/engine-sim-app.exe"
if ($LASTEXITCODE) { throw 'Application failed' }
```

Bootstrap prepares the pinned Windows dependencies and tool paths. Keep x64 and x86, compiler runtimes and build configurations separate. MinGW and Wine checks do not qualify this native application.

## macOS arm64

The Metal/CoreAudio and development-bundle recipe is prepared; native compilation and relocated execution remain pending. Use macOS 15 on Apple Silicon, Xcode 16.4, its SDK, CMake and Ninja.

Run the following in zsh.

```sh
set -e
export DEVELOPER_DIR=/Applications/Xcode_16.4.app/Contents/Developer
export CMAKE_GENERATOR=Ninja
export ES_OUT="$(cd .. && pwd)/engine-sim-output-mac"
git submodule update --init -- dependencies/submodules/simple-2d-constraint-solver
python3 -B tools/bootstrap.py --profile desktop --tests --out "$ES_OUT"
cmake --preset desktop -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
cmake --build --preset desktop -j 4
ctest --preset desktop --no-tests=error --output-on-failure
ENGINE_SIM_STAGE_MODE=local-validation cmake --install "$ES_OUT/desktop" \
  --config Release --prefix "$ES_OUT/stage"
(cd "$ES_OUT" && ./stage/EngineSim.app/Contents/MacOS/EngineSim)
```

Keep this unsigned bundle local. Signing, notarization and publication are separate work.

## Core consumers and other profiles

`core` builds Debug numerical tests. `capture` builds the Release offline characterization tool and JSON capture tests. `desktop` builds the application and native desktop tests. See [TESTING](TESTING.md) for diagnostic profiles. Visual Studio builds require the matching `--config` and CTest `-C` configuration.

The standalone Release presets `config`, `physics` and `synthesis` expose `EngineSim::Config` and `EngineSim::Json`, `EngineSim::Physics`, and `EngineSim::Synthesis`, respectively. Set `-DENGINE_SIM_CONFIG_JSON=OFF` with `config` to build just the C++ model. Standalone CMake profiles use source-local inputs and the declared system prerequisites.

A consumer that needs only the core can configure without bootstrap or GoogleTest.

```sh
cmake -S . -B /absolute/external/consumer-build -DBUILD_TESTING=OFF \
  -DENGINE_SIM_BUILD_DESKTOP=OFF
cmake --build /absolute/external/consumer-build --target engine-sim
```

For a desktop without tests, omit `--tests` from bootstrap and configure `desktop` with `-DBUILD_TESTING=OFF`. GoogleTest must stay absent. Discord presence and video recording are disabled by default and require a Windows desktop configuration. For video, initialize `dependencies/submodules/direct-to-video`, add `--with-video` to desktop bootstrap and configure with `-DDTV=ON`. The video build requires the FFmpeg libraries selected by its upstream finder. Discord opt-in additionally requires your own `ENGINE_SIM_DISCORD_APPLICATION_ID` at runtime. The default installation excludes both integrations; their redistribution prerequisites remain separate.

## Legacy converter and combined builds

Build the optional `engine-sim-convert` CLI with the `legacy-converter` preset and a fresh external output root. On Debian or Ubuntu install `bison flex libfl-dev`. On macOS select a recent Bison and Flex in `PATH`.

```sh
git submodule update --init -- dependencies/submodules/simple-2d-constraint-solver dependencies/submodules/piranha
python3 -B tools/bootstrap.py --profile converter --out "$ES_OUT"
cmake --preset legacy-converter
cmake --build --preset legacy-converter --config Release
```

The [engine migration guide](docs/engine-migration.md) shows how to run the converter in a separate process and validate the resulting JSON. To build runtime, desktop, capture and the converter together, set a fresh `ES_OUT`, initialize the selected source paths above, and prepare the union in one bootstrap.

```sh
python3 -B tools/bootstrap.py --profile runtime --with-desktop --with-capture --with-converter --tests --out "$ES_OUT"
cmake --preset runtime-test -DENGINE_SIM_BUILD_DESKTOP=ON \
  -DENGINE_SIM_BUILD_CAPTURE=ON -DENGINE_SIM_BUILD_LEGACY_CONVERTER=ON -DENGINE_SIM_BUILD_HOST_TESTS=ON
cmake --build --preset runtime-test --config Release
```

On Linux, run the dependency audits explicitly using Ninja and bubblewrap. Point `ENGINE_SIM_PROFILE_CACHE` at the verified archive cache containing GoogleTest, SDL3 and Boost. `CombinedProfileClosure` also uses the installed Flex and Bison generators and the initialized converter sources. Keep `ES_OUT` external to the checkout.

```sh
ENGINE_SIM_PROFILE_CACHE=/absolute/cache ctest --test-dir "$ES_OUT/runtime-test" \
  --output-on-failure -R '^(ProfileDependencyIsolation|CombinedProfileClosure|CsvFreeBuildProfiles|CaptureTextFormats)$'
```

## Offline rebuilding

Choose an absolute cache with `--cache /absolute/cache` during an online bootstrap. A fresh disconnected build uses `--offline --cache /absolute/cache --out /absolute/new-output` and sets `ES_OUT` to that new output. The cache must contain the verified archives for the requested profile. Initialize the pinned source trees and install native compilers and OS SDKs before going offline. For an offline Unix converter build, also prepare the host Bison, Flex and FlexLexer development header. An archive cache alone supplies none of those. Bootstrap fails on missing or altered bytes.

The installed application uses executable-relative resources and needs no network connection. Linux/Windows use `share/engine-sim/{assets,es,delta}`; the macOS bundle uses `Contents/Resources`. `runtime-inventory.json` and the included notices describe the selected payload. Keep the resource tree together when moving an installation. Use `--resources ABSOLUTE_ROOT` only for an explicitly prepared complete resource tree.

## Embedding with the C runtime SDK

The device-free SDK uses the locked solver and the checked-in JSON parser. Run the following from the checkout with the native compiler and Ninja.

```sh
set -e
export CMAKE_GENERATOR=Ninja
export ES_OUT="$(cd .. && pwd)/engine-sim-runtime-output"
export SDK_PREFIX="$ES_OUT/sdk-static"
export CONSUMER_BUILD="$ES_OUT/client-static"
git submodule update --init -- dependencies/submodules/simple-2d-constraint-solver
python3 -B tools/bootstrap.py --profile runtime --out "$ES_OUT"
cmake --preset runtime-static
cmake --build --preset runtime-static --config Release
cmake --install "$ES_OUT/runtime-static" --config Release --prefix "$SDK_PREFIX" --component RuntimeSDK
```

`runtime-shared` builds the same interface as a shared library; use a separate prefix for each variant. Both presets disable tests, desktop, capture, converter, video and Discord. The default installation contains code, the public header, CMake metadata, the JSON schema and SDK-specific notices. Set `-DENGINE_SIM_INSTALL_RUNTIME_EXAMPLE=ON` to include canonical model JSON and a synthetic impulse under `share/engine-sim-runtime/example`. Use that directory as the asset root. Existing recording and desktop redistribution restrictions still apply to their separate installer.

Consumers enable C and CXX, use `find_package(EngineSimRuntime 2.0 CONFIG REQUIRED)` and link `EngineSim::Runtime`. A C11 source includes only `<engine_sim/runtime.h>`; static linking needs the C++ final link. The imported target supplies `ES_RUNTIME_STATIC` for static builds and relative private archive dependencies. Shared builds export only the 13 C functions with ABI major 2 and library version 2.0.0. MSVC builds use the shared CRT. Put the shared SDK's DLL directory on the application's DLL search path on Windows; use an application rpath or the ordinary loader path on ELF/Darwin.

A complete independent consumer project is in `examples/runtime/consumer`:

```sh
cmake -S examples/runtime/consumer -B "$CONSUMER_BUILD" -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build "$CONSUMER_BUILD" --config Release
```

Pass the absolute JSON model path and asset directory as UTF-8 arguments. The C consumer renders 88200 native frames; the C++ consumer renders 96000 frames at 48 kHz. They use caller-owned PCM and release handles only through `es_destroy`. The [integration guide](docs/runtime-guide.md) provides the complete static/shared run commands, optional CLI/ctypes examples, control timeline, compatibility and measured limits. The [ABI reference](docs/runtime-abi.md) specifies controls, counters and recovery. No audio device is opened.

`runtime-test` provides private static unit tests. Shared SDK consumers run in separate processes against the installed shared library. `python3 -B test/runtime/installed_consumer.py --prefix "$SDK_PREFIX" --variant static --out "$FRESH_EXTERNAL_RESULT" --fixture /path/to/canonical-fixture` moves the prefix to a Unicode path and validates both consumers. Select `shared` for a shared installation. An installed example supplies the fixture when `--fixture` is omitted. On Linux, it requires bubblewrap and isolates source, producer build and desktop/device services. Other native hosts record relocation checks separately from source-hidden qualification. Run `RelocatedRuntimeStatic` and `RelocatedRuntimeShared` with `ENGINE_SIM_PROFILE_CACHE` set to exercise both minimal and combined Linux producers.

## Fleet C SDK

A runtime build also provides the `FleetSDK` install component with
`EngineSimFleet 1.0`, the `EngineSim::Fleet` target and `engine_sim/fleet.h`.
Use the `runtime-static` or `runtime-shared` preset to select linkage. Install
each variant into its own prefix. The separate `RuntimeSDK` component can
share that prefix. Use the corresponding host's x64 or arm64 toolchain
with 64-bit pointers. On Windows, use x64 MSVC with the DLL CRT (`/MD` in
Release, `/MDd` in Debug).

Enable `ENGINE_SIM_INSTALL_FLEET_EXAMPLE=ON` at configure time to make the
`FleetSDKExamples` component available. It includes the standalone consumer
project and the two MIT fixture models. The `FleetSDK` component contains
the library, public header, relocation metadata and notices.

```sh
cmake --preset runtime-static -DENGINE_SIM_INSTALL_FLEET_EXAMPLE=ON
cmake --build --preset runtime-static --config Release
cmake --install "$ES_OUT/runtime-static" --prefix /absolute/sdk --config Release --component FleetSDK
cmake --install "$ES_OUT/runtime-static" --prefix /absolute/sdk --config Release --component FleetSDKExamples
cmake -S /absolute/sdk/share/engine-sim-fleet/example -B /absolute/consumer-build -DCMAKE_PREFIX_PATH=/absolute/sdk
cmake --build /absolute/consumer-build --config Release
```

See the [Fleet integration guide](docs/fleet-guide.md) for the installed
consumer commands, callback ownership and current admission limits.
