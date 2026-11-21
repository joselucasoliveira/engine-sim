# Building Engine Simulator

Use CMake 3.25 or newer, Python 3.9 or newer, Git and a C++17 compiler. Linux development has been exercised with CMake 3.25.3 and 3.31.6, GCC 14.2 and Clang 19. See [platform evidence and limitations](docs/platforms.md) before treating a configuration as supported.

## Source and dependencies

```sh
git clone --recurse-submodules https://github.com/joselucasoliveira/engine-sim.git
cd engine-sim
git submodule update --init --recursive
git rev-parse HEAD
git submodule status --recursive
```

The public URL obtains published revisions. To test an unpublished local branch, use `git clone --no-local --branch BRANCH ABSOLUTE_LOCAL_REPOSITORY ABSOLUTE_NEW_CHECKOUT` and verify its actual HEAD. Initialize its submodules at the committed pins.

Presets require `ES_OUT` to name an absolute directory outside the checkout. Use a different output root for each compiler or diagnostic configuration. The bootstrap verifies archives against [the dependency lock](cmake/dependencies.lock.json), prepares dependencies and writes `deps.cmake` and `dependency-receipt.json` there. It does not install operating-system packages or initialize Git submodules. Scripting builds use locked Boost 1.78.0; Unix desktops build locked SDL 3.2.8. Tests acquire GoogleTest only when explicitly requested.

## Linux x86_64

Install a compiler, CMake, Ninja, Git, Python, Bison 3.2 or newer, Flex 2.6.4 and matching FlexLexer headers. For the Unix desktop, install Vulkan plus the development headers for the selected display and audio backends. On Debian/Ubuntu the package set is:

```sh
sudo apt-get install build-essential cmake ninja-build git python3 bison flex libfl-dev \
  libvulkan-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
  libxfixes-dev libxss-dev libwayland-dev wayland-protocols libxkbcommon-dev \
  libdrm-dev libgbm-dev libasound2-dev libpulse-dev
```

A functioning display, Vulkan ICD and audio endpoint are required for interactive use. From the checkout, in Bash:

```sh
set -e
export CMAKE_GENERATOR=Ninja
export ES_OUT="$(cd .. && pwd)/engine-sim-output-linux"
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

After the recursive clone, in Developer PowerShell:

```powershell
$ErrorActionPreference = 'Stop'
$env:ES_OUT = Join-Path (Get-Item ..).FullName 'engine-sim-output-windows'
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

The Metal/CoreAudio and development-bundle recipe is prepared; native compilation and relocated execution remain pending. Use macOS 15 on Apple Silicon, Xcode 16.4, its SDK, CMake and Ninja. Install recent host Bison/Flex rather than selecting the older system Bison. Record the actual tool versions.

After the same recursive clone, in zsh:

```sh
set -e
export DEVELOPER_DIR=/Applications/Xcode_16.4.app/Contents/Developer
export PATH="$(brew --prefix bison)/bin:$(brew --prefix flex)/bin:$PATH"
export CMAKE_GENERATOR=Ninja
export ES_OUT="$(cd .. && pwd)/engine-sim-output-mac"
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

`core` builds Debug numerical tests without a parser or desktop. `capture` builds the Release offline characterization tool and script tests. `desktop` builds the application and native desktop tests. See [TESTING](TESTING.md) for diagnostic profiles. Visual Studio builds require the matching `--config` and CTest `-C` configuration.

A consumer that needs only the core can configure without bootstrap or GoogleTest:

```sh
cmake -S . -B /absolute/external/consumer-build -DBUILD_TESTING=OFF \
  -DENGINE_SIM_BUILD_DESKTOP=OFF -DPIRANHA_ENABLED=OFF
cmake --build /absolute/external/consumer-build --target engine-sim
```

For a desktop without tests, omit `--tests` from bootstrap and configure `desktop` with `-DBUILD_TESTING=OFF`. GoogleTest must stay absent. Discord presence and video recording are disabled by default and require a Windows desktop configuration. Discord opt-in additionally requires your own `ENGINE_SIM_DISCORD_APPLICATION_ID` at runtime. The default installation excludes both integrations; their redistribution prerequisites remain separate.

## Offline rebuilding

Choose an absolute cache with `--cache /absolute/cache` during an online bootstrap. A fresh disconnected build uses `--offline --cache /absolute/cache --out /absolute/new-output` and sets `ES_OUT` to that new output. The cache must contain the verified archives for the requested profile. Exact recursive Git objects, initialized source trees, OS SDKs, compilers and parser tools must already be available. An archive cache alone supplies none of those. Bootstrap fails on missing or altered bytes.

The installed application uses executable-relative resources and needs no network connection. Linux/Windows use `share/engine-sim/{assets,es,delta}`; the macOS bundle uses `Contents/Resources`. `runtime-inventory.json` and the included notices describe the selected payload. Keep the resource tree together when moving an installation. Use `--resources ABSOLUTE_ROOT` only for an explicitly prepared complete resource tree.
