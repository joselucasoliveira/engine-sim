"""Replay public build/test/install entry points and retain exact-checkout evidence."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import socket
import subprocess
import sys
import time
from xml.etree import ElementTree

SOURCE = Path(__file__).resolve().parent.parent
CORE = {"GasSystemTests": 19, "FunctionTests": 5, "SynthesizerTests": 7, "RingBuffer": 3,
        "Convolution": 2, "ThreadedSynthesis": 5, "WaveReader": 8, "Resources": 5,
        "SceneReader": 4, "HostContract": 4}
SCRIPT = {"ScriptTests": 5, "Lifecycle": 5, "Displacement": 2, "Load": 8,
          "HostApplication": 8, "DesktopRun": 5, "ImpulseReference": 1}
SDL = {"DesktopInput": 8, "DesktopRender": 5, "DesktopAudio": 9}
# Additive suites are explicit: missing/extra cases still fail admission. Keep
# historical suites and their counts rather than inferring a green total.
FOUNDATION = {"RuntimeParserPreparation": 1, "Preparation": 6, "Seed": 1,
              "SeedEngines": 2, "Clock": 8, "RuntimeCAbi": 1, "RuntimeCxxAbi": 1,
              "DesktopPacing": 8, "ViewsApplication": 4, "RuntimeDesktop": 5}
CAPTURE = {"CaptureRuntime": 3, "CaptureMechanics": 1, "CaptureQuick": 1, "CaptureComparator": 1, "CaptureStress": 1, "StagedResources": 1}
PRESETS = ("core", "capture", "desktop", "desktop-windows", "asan", "tsan", "desktop-asan", "desktop-tsan", "win-asan")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expected_suites(preset, windows):
    if preset == "tsan":
        return {"ThreadedSynthesis": 5, "Load": 2}
    if preset == "desktop-tsan":
        return {"ThreadedSynthesis": 5, "Load": 2, "DesktopAudio": 9, "DesktopRun": 5}
    result = dict(CORE)
    if preset != "core":
        result.update(SCRIPT)
        result.update(FOUNDATION)
    if preset in ("capture", "desktop", "desktop-windows", "asan", "desktop-asan", "win-asan"):
        result.update(CAPTURE)
    if not windows and preset in ("desktop", "desktop-asan"):
        result.update(SDL)
    return result


def validate_tests(path, expected):
    cases = list(ElementTree.parse(path).getroot().iter("testcase"))
    names = [case.attrib["name"] for case in cases]
    if not cases or len(names) != len(set(names)) or len(cases) != sum(expected.values()):
        raise ValueError(f"test admission expected {sum(expected.values())} unique cases, found {len(cases)}")
    actual = {}
    for name in names:
        suite = name.split(".")[0]
        actual[suite] = actual.get(suite, 0) + 1
    if actual != expected or any(case.find("failure") is not None or case.find("skipped") is not None for case in cases):
        raise ValueError(f"failed, skipped or missing required suites: {actual}")
    return {"cases": len(cases), "suites": actual, "xml_sha256": sha256(path)}


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--preset", choices=PRESETS, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--expected-revision", required=True)
    parser.add_argument("--expected-arch", choices=("x86_64", "arm64"))
    parser.add_argument("--allow-dirty", action="store_true", help="precommit evidence only; never exact-checkout admission")
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--audio-mode", choices=("virtual", "device"), default="virtual")
    parser.add_argument("--retain-xlib", action="store_true", help="documented Linux X11 diagnostic lifetime condition")
    parser.add_argument("--reproducible-paths", action="store_true", help="map source/output paths with GCC/Clang")
    parser.add_argument("--c-compiler")
    parser.add_argument("--cxx-compiler")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if not args.out.is_absolute() or args.out.resolve().is_relative_to(SOURCE) or args.out.exists():
        parser.error("out must be a new absolute directory outside the source checkout")
    if args.cache and not args.cache.is_absolute():
        parser.error("cache must be absolute")
    if not re.fullmatch(r"[0-9a-f]{40}", args.expected_revision) or not 1 <= args.jobs <= 32:
        parser.error("require a full lowercase commit SHA and 1..32 build jobs")
    output = args.out.resolve()
    logs = output / "logs"
    logs.mkdir(parents=True)
    env = dict(os.environ, ES_OUT=str(output), LC_ALL="C", TZ="UTC")
    windows = platform.system() == "Windows"
    machine = platform.machine().lower()
    machine = {"amd64": "x86_64", "aarch64": "arm64"}.get(machine, machine)
    try:
        interfaces = socket.if_nameindex()
    except OSError as error:
        interfaces = {"unavailable": str(error)}
    report = {"preset": args.preset, "requested_revision": args.expected_revision, "source": str(SOURCE),
              "system": platform.platform(), "architecture": machine, "python": platform.python_version(),
              "offline_bootstrap": args.offline, "network_interfaces": interfaces,
              "commands": [], "status": "running", "native_hardware_qualification": "Pending independent native input, sink/loopback and hardware evidence."}
    started = time.monotonic()

    def command(label, argv, *, environment=None, timeout=1800, capture=False):
        path = logs / (label + ".log")
        with path.open("wb") as stream:
            try:
                result = subprocess.run([str(value) for value in argv], cwd=SOURCE, env=environment or env,
                                        stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
            except subprocess.TimeoutExpired:
                stream.flush()
                report["commands"].append({"argv": list(map(str, argv)), "cwd": str(SOURCE), "exit": None,
                    "timeout_seconds": timeout, "log": path.name, "sha256": sha256(path)})
                raise RuntimeError(f"{label} timed out after {timeout}s; see {path}") from None
        report["commands"].append({"argv": list(map(str, argv)), "cwd": str(SOURCE), "exit": result.returncode,
                                   "log": path.name, "sha256": sha256(path)})
        print(label, result.returncode, flush=True)
        if result.returncode:
            raise RuntimeError(f"{label} failed; see {path}")
        return path.read_text(errors="replace").strip() if capture else None

    try:
        report["revision"] = command("revision", ["git", "rev-parse", "HEAD"], capture=True)
        report["tree"] = command("tree", ["git", "rev-parse", "HEAD^{tree}"], capture=True)
        report["working_status"] = command("working-status", ["git", "--no-optional-locks", "status", "--porcelain", "--untracked-files=all", "--ignore-submodules=none"], capture=True)
        if report["revision"] != args.expected_revision or (report["working_status"] and not args.allow_dirty):
            raise ValueError("checkout differs from the requested clean exact revision")
        report["exact_clean_checkout"] = not bool(report["working_status"])
        if args.expected_arch and machine != args.expected_arch:
            raise ValueError(f"expected {args.expected_arch}, found {machine}")
        command("admission-tests", [sys.executable, "-B", str(SOURCE / "test/verify_build_tests.py"), "--out", str(output / "admission-tests")])
        report["submodules"] = command("submodules", ["git", "submodule", "status", "--recursive"], capture=True)
        env["SOURCE_DATE_EPOCH"] = command("source-date-epoch", ["git", "show", "-s", "--format=%ct", "HEAD"], capture=True)
        report["runner"] = {name: env.get(name) for name in ("ImageOS", "ImageVersion", "RUNNER_OS", "RUNNER_ARCH", "GITHUB_SHA", "GITHUB_EVENT_NAME", "VSCMD_VER", "WindowsSDKVersion", "DEVELOPER_DIR")}
        command("cmake-version", ["cmake", "--version"])
        command("git-version", ["git", "--version"])
        if windows:
            probe = output / "compiler-probe.cpp"
            probe.write_text("int main() { return 0; }\n")
            command("compiler-version", ["cl.exe", "/nologo", "/Bv", "/c", str(probe), "/Fo" + str(output / "compiler-probe.obj")])
        else:
            command("compiler-version", [args.cxx_compiler or ("clang++" if "asan" in args.preset else "c++"), "--version"])
            if args.preset != "core":
                command("flex-version", ["flex", "--version"])
                command("bison-version", ["bison", "--version"])
            if platform.system() == "Darwin":
                command("xcode-version", ["xcodebuild", "-version"])
                command("sdk-version", ["xcrun", "--show-sdk-version"])
        profile = "core" if args.preset == "core" else ("desktop" if "desktop" in args.preset or args.preset == "win-asan" else "capture")
        bootstrap = [sys.executable, "-B", str(SOURCE / "tools/bootstrap.py"), "--profile", profile, "--tests", "--out", str(output)]
        if args.cache:
            bootstrap += ["--cache", str(args.cache)]
        if args.offline:
            bootstrap.append("--offline")
        command("bootstrap", bootstrap)
        report["dependency_receipt_sha256"] = sha256(output / "dependency-receipt.json")
        configure = ["cmake", "--preset", args.preset, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
        # Desktop CI also runs the public quick/stress characterization and staged-resource cases.
        if profile == "desktop":
            configure.append("-DENGINE_SIM_BUILD_CAPTURE=ON")
        if args.c_compiler:
            configure.append("-DCMAKE_C_COMPILER=" + args.c_compiler)
        if args.cxx_compiler:
            configure.append("-DCMAKE_CXX_COMPILER=" + args.cxx_compiler)
        if args.reproducible_paths:
            if windows:
                raise ValueError("reproducible-paths currently requires GCC/Clang; Windows binary identity remains separately unassessed")
            mappings = [(SOURCE, "."), (output, "build")]
            prepared = json.loads((output / "dependency-receipt.json").read_text())["cmake"]
            for key, name in (("ENGINE_SIM_GOOGLETEST_SOURCE", "googletest"), ("BOOST_ROOT", "boost"), ("ENGINE_SIM_SDL3_SOURCE", "sdl3")):
                if key in prepared:
                    mappings.append((Path(prepared[key]), "dependency/" + name))
            flags = " ".join(f"-ffile-prefix-map={path}={name}" for path, name in mappings)
            report["reproducible_path_maps"] = [[str(path), name] for path, name in mappings]
            configure += ["-DCMAKE_C_FLAGS=" + flags, "-DCMAKE_CXX_FLAGS=" + flags]
        command("configure", configure)
        build_environment = dict(env, ASAN_OPTIONS="detect_leaks=0")
        command("build", ["cmake", "--build", "--preset", args.preset, "-j", str(args.jobs)], environment=build_environment)
        if "asan" in args.preset:
            env.update(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:strict_string_checks=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        if "tsan" in args.preset:
            env["TSAN_OPTIONS"] = "halt_on_error=1:exitcode=66"
        if args.retain_xlib:
            if platform.system() != "Linux" or "asan" not in args.preset:
                raise ValueError("retain-xlib applies only to the documented Linux address-sanitizer desktop tuple")
            ctypes.CDLL("libX11.so.6")
            paths = {Path(line.split()[-1]).resolve() for line in Path("/proc/self/maps").read_text().splitlines() if "/libX11.so" in line}
            if len(paths) != 1:
                raise ValueError("cannot identify the exact loaded Xlib lifetime control")
            library = paths.pop()
            env["LD_PRELOAD"] = str(library)
            report["xlib_lifetime_condition"] = {"path": str(library), "sha256": sha256(library), "scope": "Retain Xlib process-global caches until LeakSanitizer exit; no checker or allocation is suppressed."}
        env["ENGINE_SIM_STAGE_EVIDENCE"] = str(output / "staged-test-evidence")
        tests = ["ctest", "--preset", args.preset, "--no-tests=error", "--output-on-failure", "--output-junit", str(logs / "tests.xml")]
        if args.preset == "tsan":
            tests += ["-L", "threaded"]
        if args.preset == "desktop-tsan":
            tests += ["-L", "desktop-audio|threaded|desktop-smoke-unit"]
        command("tests", tests)
        report["tests"] = validate_tests(logs / "tests.xml", expected_suites(args.preset, windows))
        build = output / args.preset
        if args.preset != "core":
            if not windows:
                command("analysis", ["cmake", "--build", "--preset", args.preset, "--target", "engine-sim-analysis"])
            stage_environment = dict(env, ENGINE_SIM_STAGE_MODE="local-validation")
            configuration = "RelWithDebInfo" if "san" in args.preset else "Release"
            command("install", ["cmake", "--install", str(build), "--config", configuration, "--prefix", str(output / "stage")], environment=stage_environment)
            report["stage_inventory_sha256"] = sha256(output / "stage/runtime-inventory.json")
            if args.smoke:
                if profile != "desktop":
                    raise ValueError("smoke requires a desktop preset")
                app = output / ("stage/EngineSim.app/Contents/MacOS/EngineSim" if platform.system() == "Darwin" else "stage/bin/engine-sim-app" + (".exe" if windows else ""))
                script = output / ("stage/EngineSim.app/Contents/Resources/assets/main.mr" if platform.system() == "Darwin" else "stage/share/engine-sim/assets/main.mr")
                command("desktop-smoke", [sys.executable, "-B", str(SOURCE / "tools/run_desktop_smoke.py"), "--app", str(app),
                    "--script", str(script), "--out", str(output / "smoke"), "--audio-mode", args.audio_mode])
                report["smoke_receipt_sha256"] = sha256(output / "smoke/receipt.json")
                report["application_sha256"] = sha256(app)
                if platform.system() == "Linux":
                    command("runtime-linkage", ["ldd", str(app)])
                    command("runtime-dynamic-tags", ["readelf", "-d", str(app)])
                elif platform.system() == "Darwin":
                    command("runtime-linkage", ["otool", "-L", str(app)])
                    command("runtime-architecture", ["lipo", "-archs", str(app)])
                else:
                    command("runtime-linkage", ["dumpbin", "/DEPENDENTS", str(app)])
        report["status"] = "pass"
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as failure:
        report.update(status="fail", error=str(failure))
        print(str(failure), file=sys.stderr)
    finally:
        # Upload diagnostic records and metrics without copying raw PCM, runtime
        # assets or the private installation into the workflow artifact.
        try:
            for source, destination in (
                    (output / "dependency-receipt.json", logs / "dependency-receipt.json"),
                    (output / "stage/runtime-inventory.json", logs / "runtime-inventory.json")):
                if source.is_file():
                    shutil.copyfile(source, destination)
            for source, label, extensions in (
                    (output / args.preset / "analysis", "analysis", {".json", ".log", ".plist"}),
                    (output / args.preset / "Testing/Temporary", "ctest", {".log"}),
                    (output / args.preset / "tools/capture", "capture", {".json"}),
                    (output / "smoke", "smoke", {".json", ".log", ".txt"})):
                if source.is_dir():
                    for file in sorted(source.rglob("*")):
                        if file.is_file() and file.suffix in extensions:
                            destination = logs / label / file.relative_to(source)
                            destination.parent.mkdir(parents=True, exist_ok=True)
                            shutil.copyfile(file, destination)
        except OSError as failure:
            report.update(status="fail", error=f"could not retain diagnostic evidence: {failure}")
        report["wall_seconds"] = time.monotonic() - started
        report["diagnostic_environment"] = {name: env.get(name) for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS", "LD_PRELOAD", "LP_NUM_THREADS", "SDL_VIDEO_DRIVER", "SDL_AUDIO_DRIVER", "VK_DRIVER_FILES", "DBUS_SESSION_BUS_ADDRESS", "SOURCE_DATE_EPOCH")}
        report["log_sha256"] = {str(path.relative_to(logs)): sha256(path) for path in sorted(logs.rglob("*")) if path.is_file()}
        (logs / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
