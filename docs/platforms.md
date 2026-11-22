# Platform evidence and limits

These are separate feature checks. A source recipe, virtual endpoint or cross-compilation result does not establish native desktop or hardware qualification. There is no published release qualification for this revision.

| Configuration | Implemented path | Exercised evidence | Still pending |
| --- | --- | --- | --- |
| Linux x86_64, Debian 13, GCC 14.2 Release and Clang 19 diagnostics, CMake 3.25.3/3.31.6 | Existing engine views and controls through SDL 3.2.8, Vulkan and SDL audio | Numerical/script/lifetime tests; X11/Xvfb and lavapipe GPU readbacks; queued input; failed/successful reload; installed and source-hidden relocated runs; explicit virtual audio; strict memory diagnostics; separate worker TSan | Interactive hardware/input parity, hardware hot-plug and default-device migration, physical loopback and human listening; default GPU/bus TSan cells below |
| Linux PulseAudio default NVIDIA AD102 HDMI endpoint | SDL conversion and native stream feeding | Endpoint reports 48000 Hz stereo S32 and a 1024-frame quantum; actual source/backend progress and nonzero samples | Physical output recording, acoustic latency and listening. Stream consumption is not proof of speaker output |
| Windows x64, VS 2022/v143 and Windows SDK | WinMain, original Delta Studio D3D11/DirectSound adapter; explicit virtual sink; install and replay recipes | Portable core/cross checks and supplemental source checks only | Native Debug/Release/MSVC ASan, drawable/input/device behavior, DLL imports and relocated installation |
| macOS 15 arm64, Xcode 16.4 | SDL Metal/CoreAudio adapter and unsigned development bundle recipe | Source/data preparation only | Native compile/link, CTest, input/presentation/audio, architecture/linkage and relocated bundle execution |
| Ubuntu 24.04 GCC 13/Clang 18, Windows Server 2022 x64, hosted macOS 15 arm64 | Pinned workflow definitions with exact-revision and architecture checks | Public commands rehearsed locally; workflow syntax and admission controls checked | Actual hosted jobs and their exact-revision artifacts; a workflow definition is not a green run |

Other architectures, Web targets and other toolchains are outside qualification. Wine and MinGW attempts do not replace a native Windows result. Optional Discord/video integrations are excluded from the default runtime and parity claims.

## Diagnostic conditions

Memory checks exercise ASan, UBSan and LeakSanitizer together, with separate TSan builds. The X11 memory tuple explicitly retains the exact Xlib library through process exit so its global caches remain reachable; allocation reports are not suppressed.

The exercised lavapipe build reports a default-worker TSan race also reproduced by a standalone SDL GPU shader program that links no simulator code. A separate real-bus SDL-only control reproduces libdbus lock ordering. Those external runtime cells remain failing. An alternate whole-app TSan run with `LP_NUM_THREADS=0` passes while the solver, synthesizer and audio workers remain active. Worker-only TSan also passes with the optional desktop bus absent. These conditions qualify only those recorded runs. They do not establish that a default GPU driver or desktop bus is free of concurrency defects.

## Runtime data and display text

The app uses the inherited scene, engine scripts, impulse responses, font and original solver/synthesis chain. Source audio is mono S16 at 44.1 kHz. SDL may convert to the device's format at the final boundary. Software counters describe source submission and input consumption; physical playback, latency and listening require independent evidence.

The inherited Silkscreen font has glyphs 32 through 127. UI text keeps those glyphs, normalizes line endings/tabs and displays one `?` for an unsupported valid UTF-8 code point. Malformed bytes are handled safely. Raw load diagnostics retain their complete text in the model and standard error. An unsuccessful reload retains the current running engine and its audio session; a later successful reload replaces it.

The runtime allowlist includes complete notices for its selected inputs. The root MIT license does not establish separate recording redistribution rights. Original impulse-recording clearance remains unresolved, so default redistribution staging is blocked. Explicit local-validation stages preserve the original bytes and identify their private purpose. Signing, publication and distribution require separate decisions.

## Device-free runtime SDK

Local Linux x86_64/GCC 14.2 and Clang 19 checks exercise static/shared SDK links,
source-hidden Unicode relocation, C11/C++17 consumers, standalone ctypes, both
shaft modes and both output rates. Separate full memory/leak/UB and thread
selections exercise preparation, reset, retirement, coherent fault prefixes and
independent concurrent owners. The optimized static benchmark is a separate
CPU observation; it does not qualify audio callback deadlines or hardware.

The manual runtime SDK workflow defines required producer and separate clean
consumer jobs for Ubuntu 24.04/GCC 13, Windows Server 2022/VS 2022 x64 and macOS
15 arm64/Xcode 16.4, plus separate Linux and MSVC diagnostics. It verifies actual
OS, machine, compiler and SDK and retains exact commit/run and file identities.
The image catalog and local workflow/transport checks are source evidence;
actual hosted runs and native Windows/macOS execution remain pending. The
original desktop, driver/bus, physical audio and recording-rights limits above
remain separate.
