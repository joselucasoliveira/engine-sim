"""Build the standalone profiles and compare the SDK closure in mixed producers."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

from installed_consumer import file_sha256
import physics_projection as physics
import synthesis_projection as synthesis


ROOT_FILES = ("CMakeLists.txt", "CMakePresets.json", "cmake/Diagnostics.cmake",
              "cmake/dependencies.lock.json", "tools/bootstrap.py")
SOLVER = "simple-2d-constraint-solver"
SOLVER_FILES = ("dependencies/CMakeLists.txt", "dependencies/submodules/CMakeLists.txt",
                "cmake/LockedSources.cmake", "cmake/SolverCompatibility.h")
JSON_FILES = ("modules/json", "schemas/engine-config-v1.schema.json", "cmake/compile_config_schema.py")
SDK_FILES = ("LICENSE", "THIRD_PARTY_NOTICES.md", "cmake/RuntimeSdk.cmake", "cmake/runtime.elf.exports",
             "cmake/EngineSimRuntimeConfig.cmake.in", "cmake/InstallSdkPreflight.cmake.in", "cmake/runtime-sdk-notices.md",
             "modules/assembly", "include/engine_sim/runtime.h",
             *(f"src/runtime/{path}" for path in (
                 "api.cpp", "api_text.hpp", "json_runtime.cpp", "json_runtime.hpp", "json_session.hpp",
                 "runtime.cpp", "runtime.hpp", "concrete_validation.hpp", "shaft_drive.hpp")))
CORE = ("audio_buffer", "feedback_comb_filter", "sample_clock", "pull_engine", "wave_reader")
OPTIONAL_INPUT = re.compile(
    r"(?i)(?:/|\b)(?:piranha|piranha@|csv-io|delta-studio|direct-to-video|googletest|"
    r"boost(?:_[0-9]+|/filesystem)?|boostfilesystem\.cmake|sdl[23](?:_image)?|"
    r"lib(?:boost|SDL|GL|EGL|Vulkan|wayland|X11|asound|pulse|pipewire|avcodec|avformat|gtest)"
    r"[^\s/]*|discord|FFmpeg|d3dx|FlexLexer\.h|host-resources|engine-sim-(?:script-interpreter|convert|capture|frontend|desktop-runtime))"
    r"(?=[/._\s\"'@-]|$)")
OPTIONAL_SYMBOL = re.compile(r"piranha::|boost::filesystem|\bSDL_[A-Za-z]|\bDiscord_[A-Za-z]|"
                             r"\bav(codec|format)_|\bydsWindows|CompiledScene::|processArguments\(")
BASE_ENV = ("CC", "CXX", "PATH", "BISON_PKGDATADIR", "M4", "LC_ALL", "TZ")
SEARCH_ENV = ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "CMAKE_PREFIX_PATH",
              "CMAKE_LIBRARY_PATH", "CMAKE_INCLUDE_PATH", "CMAKE_MODULE_PATH", "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR")
NO_NETWORK = re.compile(r"(socket\(AF_INET6?[,)]|sa_family=AF_INET6?[,}])")
PARSER_PROCESS = re.compile(r'execve\("[^"\n]*/(?:flex|bison|b2)(?:[".-])')


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def sources(names):
    return tuple(f"src/{name}.cpp" for name in names)


def headers(names):
    return tuple(f"include/{name}.h" for name in names)


class Audit:
    def __init__(self, args, root):
        self.args = args
        self.root = root
        self.commands = []
        self.env = {key: value for key, value in os.environ.items() if key not in SEARCH_ENV}

    def run(self, label, argv, *, cwd=None, env=None, expected=None, trace=False, minimal=False):
        argv = list(map(str, argv))
        cwd = cwd or self.args.source
        self.root.mkdir(parents=True, exist_ok=True)
        log = f"{len(self.commands) + 1:03d}-{label}.log"
        tracer = os.environ.get("ENGINE_SIM_STRACE") if trace else None
        system_calls = self.root / (Path(log).stem + ".strace")
        if tracer:
            argv = [tracer, "-f", "-q", "-s", "4096", "-e", "trace=file,process,network", "-o", str(system_calls), *argv]
        result = subprocess.run(argv, cwd=cwd, env=env if env is not None else self.env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (self.root / log).write_bytes(result.stdout)
        self.commands.append({"argv": argv, "cwd": str(cwd), "exit": result.returncode, "log": log})
        write_json(self.root / "commands.json", self.commands)
        if expected is None and result.returncode:
            raise RuntimeError(f"{label} exited {result.returncode}. See {self.root / log}\n"
                               + result.stdout[-5000:].decode(errors="replace"))
        if expected is not None:
            assert result.returncode and expected.encode() in result.stdout, (label, result.stdout)
        if tracer:
            with system_calls.open(errors="replace") as stream:
                assert not any(NO_NETWORK.search(line) or (minimal and PARSER_PROCESS.search(line))
                               for line in stream), f"Unexpected network or parser tool activity in {system_calls}"
            if system_calls.stat().st_size > 32 * 1024 * 1024:
                compressed = system_calls.with_suffix(".strace.gz")
                with system_calls.open("rb") as raw, gzip.open(compressed, "wb", compresslevel=4) as destination:
                    shutil.copyfileobj(raw, destination)
                system_calls.unlink()
        return result.stdout.decode(errors="replace")

    def projection(self, name, paths, gitlinks=()):
        checkout = self.root / name / "source"
        checkout.parent.mkdir()
        # Preserve the actual parent revision for bootstrap's receipt. Working files
        # are copied explicitly and submodule objects have independent local storage.
        self.run("parent", ["git", "clone", "--quiet", "--local", "--shared", "--no-checkout",
                            self.args.source, checkout])
        for relative in sorted(set((*ROOT_FILES, *paths))):
            origin = self.args.source / relative
            target = checkout / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if origin.is_dir():
                shutil.copytree(origin, target)
            else:
                shutil.copyfile(origin, target)
        for name in gitlinks:
            relative = Path("dependencies/submodules") / name
            self.run(name, ["git", "clone", "--quiet", "--local", "--no-hardlinks",
                            self.args.source / relative, checkout / relative])
        inventory = {path.relative_to(checkout).as_posix(): file_sha256(path)
                     for path in sorted(checkout.rglob("*"))
                     if path.is_file() and ".git" not in path.relative_to(checkout).parts}
        write_json(checkout.parent / "source-files.json", inventory)
        return checkout

    def bootstrap(self, source, output, profile, flags=(), cache=None):
        # None selects a real empty archive cache for archive-free profiles.
        cache = cache or output.parent / "empty-cache"
        args = ["--offline", "--cache", cache, "--out", output, "--profile", profile, *flags]
        self.run(f"bootstrap-{profile}", [sys.executable, "-B", source / "tools/bootstrap.py", *args], cwd=source,
                 trace=True, minimal=not flags and profile != "converter")
        receipt = read_json(output / "dependency-receipt.json")
        assert receipt["offline"] and receipt["profile"] == profile
        return receipt

    def configure(self, source, out, preset, *options):
        build = out / preset
        query = build / ".cmake/api/v1/query"
        query.mkdir(parents=True)
        (query / "codemodel-v2").touch()
        cc = f"-DCMAKE_CXX_COMPILER={self.args.compiler}"
        # Always use the same generator, compiler, output policy and locale for
        # compared producers. No package discovery or fetch can use network.
        env = dict(self.env, ES_OUT=str(out), LC_ALL="C", TZ="UTC")
        traces = [f"--trace-source={source / path}" for path in
                  ("CMakeLists.txt", "cmake/LockedSources.cmake", "cmake/BoostFilesystem.cmake",
                   "cmake/Piranha.cmake", "cmake/RuntimeSdk.cmake")]
        optional = preset == "legacy-converter" or "-DENGINE_SIM_BUILD_LEGACY_CONVERTER=ON" in options
        off = () if optional else tuple(f"-DCMAKE_DISABLE_FIND_PACKAGE_{package}=ON" for package in
                                        ("Boost", "FLEX", "BISON", "SDL2", "SDL2_image", "SDL3"))
        self.run(f"configure-{preset}", [self.args.cmake, "--preset", preset, "-G", "Ninja",
                cc, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
                "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF", "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF",
                "--trace-expand", "--trace-format=json-v1", f"--trace-redirect={build}/configure-trace.jsonl",
                *traces, *off, *options], cwd=source, env=env, trace=True, minimal=not optional)
        return build

    def build(self, build, *targets):
        args = ["--target", *targets] if targets else []
        self.run(f"build-{build.name}", [self.args.cmake, "--build", build, "--config", "Release",
                                        "--parallel", self.args.jobs, "--verbose", *args])

    def module(self, name, profile, paths, target, consumer, gitlinks=(), options=()):
        source = self.projection(name, paths, gitlinks)
        out = source.parent / "out"
        receipt = self.bootstrap(source, out, profile)
        assert set(receipt["git"]) == set(gitlinks) and not receipt["archives"], receipt
        client = source.parent / "consumer.cmake"
        client.write_text(
            f'add_executable(profile-consumer "{source / consumer}")\n'
            f'target_link_libraries(profile-consumer PRIVATE {target})\n'
            'target_compile_features(profile-consumer PRIVATE cxx_std_17)\n'
            'set_target_properties(profile-consumer PROPERTIES CXX_EXTENSIONS OFF)\n', encoding="utf-8")
        build = self.configure(source, out, profile, f"-DCMAKE_PROJECT_INCLUDE={client}", *options)
        self.build(build, "profile-consumer")
        compile_db = (build / "compile_commands.json").read_text(encoding="utf-8")
        assert not OPTIONAL_INPUT.search(compile_db), name
        self.run(f"dependencies-{name}", ["ninja", "-C", build, "-t", "deps"])
        return build / "bin/profile-consumer"

    def modules(self):
        config = self.module("model", "config", ("modules/config", "test/config/public_header.cpp"),
                             "EngineSim::Config", "test/config/public_header.cpp",
                             options=("-DENGINE_SIM_CONFIG_JSON=OFF",))
        self.run("model-consumer", [config], cwd=config.parent)
        json_client = self.module("json", "config", ("modules/config", *JSON_FILES, "examples/config/roundtrip.cpp"),
                                  "EngineSim::Json", "examples/config/roundtrip.cpp")
        work = self.root / "json"
        valid = self.args.source / "examples/runtime/fixture/main.json"
        self.run("json-consumer", [json_client, valid, work / "canonical.json"], cwd=work)
        self.run("json-repeat", [json_client, work / "canonical.json", work / "repeat.json"], cwd=work)
        assert file_sha256(work / "canonical.json") == file_sha256(work / "repeat.json")
        invalid = work / "invalid.json"
        invalid.write_text("{}\n", encoding="utf-8")
        self.run("json-rejection", [json_client, invalid, work / "unexpected.json"], cwd=work, expected="CFG_SHAPE /format_version")
        assert not (work / "unexpected.json").exists()
        observer = self.module("physics", "physics", (*SOLVER_FILES, "modules/config", "modules/physics",
                               "modules/assembly/src", *sources(physics.SOURCES), *headers(physics.HEADERS),
                               "test/runtime/physics_consumer.cpp", "test/runtime/physics_scenario.hpp"),
                               "EngineSim::Physics", "test/runtime/physics_consumer.cpp", (SOLVER,))
        self.run("physics-consumer", [observer, self.root / "physics/trace.le64"], cwd=observer.parent)
        dsp = self.module("synthesis", "synthesis", ("modules/synthesis", *sources(synthesis.SOURCES),
                          *headers(synthesis.HEADERS), *(f"test/runtime/{name}" for name in synthesis.CONSUMER)),
                          "EngineSim::Synthesis", "test/runtime/synthesis_consumer.cpp")
        self.run("synthesis-consumer", [dsp], cwd=dsp.parent)

    def sdk_source(self, converter=False):
        extra = ("scripting", "tools/legacy", "cmake/Piranha.cmake", "cmake/PiranhaPortability.cmake",
                 "cmake/LegacyConverterIdentity.cmake", "cmake/BoostFilesystem.cmake", "cmake/DiagnosticBoost.cmake",
                 "src/resource_paths.cpp", "include/resource_paths.h") if converter else ()
        return self.projection("converter" if converter else "runtime",
                               (*SOLVER_FILES, *JSON_FILES, *SDK_FILES, *extra,
                                "modules/config", "modules/physics", "modules/synthesis",
                                *sources((*CORE, *physics.SOURCES, *synthesis.SOURCES)),
                                *headers((*CORE, *physics.HEADERS, *synthesis.HEADERS, "engine_session"))),
                               (SOLVER, "piranha") if converter else (SOLVER,))

    def converter(self, cache):
        source = self.sdk_source(converter=True)
        out = source.parent / "out"
        receipt = self.bootstrap(source, out, "converter", cache=cache)
        assert set(receipt["git"]) == {SOLVER, "piranha"} and set(receipt["archives"]) == {"boost"}, receipt
        build = self.configure(source, out, "legacy-converter")
        self.build(build, "engine-sim-convert")
        self.run("converter-positive-and-negative", [sys.executable, "-B",
                 self.args.source / "test/script/legacy_converter.py", build / "bin/engine-sim-convert",
                 self.args.source / "examples/runtime/fixture", build, "cli"])

    def sdk(self, source, out, variant, mixed=False, converter=True):
        options = []
        if variant == "shared":
            path = out / "runtime-shared/sdk-link.map"
            options.append(f"-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-Map,{path}")
        if mixed:
            options.extend(("-DENGINE_SIM_BUILD_DESKTOP=ON", "-DENGINE_SIM_BUILD_CAPTURE=ON",
                            f"-DENGINE_SIM_BUILD_LEGACY_CONVERTER={'ON' if converter else 'OFF'}",
                            f"-DBUILD_TESTING={'ON' if variant == 'static' else 'OFF'}",
                            f"-DENGINE_SIM_BUILD_HOST_TESTS={'ON' if variant == 'static' else 'OFF'}"))
        build = self.configure(source, out, "runtime-" + variant, *options)
        targets = ["engine-sim-runtime"]
        if mixed:
            targets += ["engine-sim-app", "engine-sim-capture"]
            if converter:
                targets.append("engine-sim-convert")
            if variant == "static":
                targets += ["engine-sim-host-test", "engine-sim-pacing-test"]
        self.build(build, *targets)
        if mixed and variant == "static":
            self.run("mixed-host-tests", [self.args.ctest, "--test-dir", build, "--no-tests=error",
                                        "--output-on-failure", "-R",
                                        r"^((HostContract|RuntimeDesktop)\.|CaptureCanonicalReplay$|CaptureNoLegacyAdapter$)"])
        prefix = build / "stage"
        self.run("install-sdk", [self.args.cmake, "--install", build, "--config", "Release",
                                 "--prefix", prefix, "--component", "RuntimeSDK"])
        summary = self.inspect_sdk(source, build, prefix, variant)
        consumer = build / "installed-consumer"
        fixture = build / "fixture"
        fixture.mkdir()
        manifest = read_json(self.args.source / "cmake/runtime-example.json")
        for item in manifest["files"]:
            original = self.args.source / "examples/runtime/fixture" / item["path"]
            assert file_sha256(original) == item["sha256"]
            shutil.copyfile(original, fixture / item["path"])
        self.run(f"relocate-{variant}", [sys.executable, "-B", self.args.source / "test/runtime/installed_consumer.py",
                 "--prefix", prefix, "--variant", variant, "--out", consumer, "--fixture", fixture])
        proof = read_json(consumer / "inside-result.json")
        assert proof["status"] == "pass", proof
        if os.environ.get("ENGINE_SIM_STRACE"):
            for name in ("runtime-c-consumer", "runtime-cxx-consumer", "runtime-matrix-c", "runtime-matrix-cxx"):
                trace = (consumer / f"{name}.strace").read_text(errors="replace")
                assert trace.count("execve(") == 1 and not NO_NETWORK.search(trace)
                assert "main.json" in trace and ".mr\"" not in trace and not OPTIONAL_INPUT.search(trace)
        summary["consumer"] = [{key: value for key, value in row.items()
                                if key in ("native_matrix", "foreign_matrix", "imported_properties")}
                               for row in proof["commands"] if any(key in row for key in
                               ("native_matrix", "foreign_matrix", "imported_properties"))]
        return summary

    def inspect_sdk(self, source, build, prefix, variant):
        def normalize(value):
            return value.replace(str(build), "<build>").replace(str(source), "<source>")

        replies = build / ".cmake/api/v1/reply"
        model = read_json(next(replies.glob("codemodel-v2-*.json")))["configurations"][0]
        by_id = {item["id"]: read_json(replies / item["jsonFile"]) for item in model["targets"]}
        closure = {}

        def visit(target):
            if target["name"] not in closure:
                closure[target["name"]] = target
                for child in target.get("dependencies", []):
                    visit(by_id[child["id"]])

        visit(next(target for target in by_id.values() if target["name"] == "engine-sim-runtime"))
        graph = {
            name: sorted(by_id[edge["id"]]["name"] for edge in target.get("dependencies", []))
            for name, target in closure.items()
        }
        raw = read_json(build / "compile_commands.json")
        commands = [row for row in raw if any(f"CMakeFiles/{name}.dir/" in row["command"] for name in closure)]
        assert commands and any(row["file"].endswith("/src/runtime/api.cpp") for row in commands)
        objects = [shlex.split(row["command"])[shlex.split(row["command"]).index("-o") + 1] for row in commands]
        depfiles = self.run("sdk-depfiles", ["ninja", "-C", build, "-t", "deps", *objects])
        assert all(f"{obj}: #" in depfiles for obj in objects)
        dependencies = {normalize(lines[0].split(": #", 1)[0]): sorted(
            normalize(line.strip()) for line in lines[1:] if line.strip())
            for block in depfiles.strip().split("\n\n") if (lines := block.splitlines())}
        assert not OPTIONAL_INPUT.search(depfiles)
        compile_inputs = sorted(normalize(row["command"]) for row in commands)
        assert not OPTIONAL_INPUT.search("\n".join(compile_inputs))
        link_commands = self.run("sdk-ninja-commands", ["ninja", "-C", build, "-t", "commands", "engine-sim-runtime"])
        assert link_commands and not OPTIONAL_INPUT.search(link_commands)
        product_inputs = ("engine-sim-runtime-backend", "engine-sim-runtime-json-private", "engine-sim",
                          "engine-sim-assembly", "engine-sim-json", "engine-sim-config", SOLVER)
        assert set(product_inputs) <= set(closure) and not OPTIONAL_INPUT.search(" ".join(closure))
        owned = [item for item in sorted(prefix.rglob("*")) if item.is_file()]
        metadata = {p.relative_to(prefix).as_posix(): file_sha256(p) for p in owned
                    if p.suffix not in (".a", ".so") and ".so." not in p.name}
        symbols = {}
        for lib in owned:
            key = lib.relative_to(prefix).as_posix()
            if lib.suffix == ".a":
                members = self.run("archive-members", ["ar", "t", lib])
                exported = self.run("archive-symbols", ["nm", "-C", lib])
                assert members and not OPTIONAL_INPUT.search(members) and not OPTIONAL_SYMBOL.search(exported)
                symbols[key] = {"members": members.splitlines(), "symbols_sha256": digest(exported)}
            if lib.name == "libengine-sim-runtime.so.2.0.0":
                exported = self.run("shared-symbols", ["nm", "-D", "--defined-only", lib])
                dynamic = self.run("shared-dependencies", ["readelf", "-d", lib])
                link_map = (build / "sdk-link.map").read_text(errors="replace")
                assert link_map and not OPTIONAL_INPUT.search(link_map) and not OPTIONAL_SYMBOL.search(link_map)
                assert not OPTIONAL_INPUT.search(dynamic)
                assert "libengine-sim-runtime.so.2" in dynamic
                symbols[key] = {"exports": sorted(row.split()[-1] for row in exported.splitlines()),
                                "needed": re.findall(r"Shared library: \[(.*?)\]", dynamic),
                                "objects": sorted(set(re.findall(r"([A-Za-z_][-A-Za-z_0-9.]*\.a\([^()]+\.o\))",
                                                                  link_map)))}
                assert symbols[key]["objects"], "Empty shared linker object evidence"
        assert symbols
        result = {"targets": graph, "compile": compile_inputs, "dependencies": dependencies,
                  "files": [p.relative_to(prefix).as_posix() for p in owned],
                  "metadata": metadata, "binary_signatures": symbols,
                  "binary_sha256": {p.relative_to(prefix).as_posix(): file_sha256(p) for p in owned
                                    if p.relative_to(prefix).as_posix() not in metadata}}
        write_json(build / "sdk-closure.json", result)
        return result


def digest(value):
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def minimal(audit):
    source = audit.sdk_source()
    out = source.parent / "out"
    assert set(audit.bootstrap(source, out, "runtime")["git"]) == {SOLVER}
    return {variant: audit.sdk(source, out, variant) for variant in ("static", "shared")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("isolation", "combined"))
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--deps-root", type=Path, required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--ctest", required=True)
    parser.add_argument("--jobs", default=os.environ.get("ENGINE_SIM_PROFILE_JOBS", "3"))
    args = parser.parse_args()
    output = os.environ.get("ENGINE_SIM_GATE_OUTPUT")
    if output:
        root = Path(output) / "closure/linux"
        root.mkdir(parents=True, exist_ok=True)
    else:
        root = args.deps_root / "profile-audit"
        root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=root, prefix=f"{args.mode}-") as temporary:
        work = Path(temporary)
        if output:
            # An external step attempt retains all build products and first failures.
            retained = root / args.mode
            work.rename(retained)
            work = retained
        evidence = Audit(args, work)
        write_json(work / "environment.json", {key: os.environ.get(key) for key in BASE_ENV})
        if args.mode == "isolation":
            evidence.modules()
            write_json(work / "summary.json", minimal(evidence))
        else:
            cached = root / "isolation/summary.json"
            baseline = read_json(cached) if output and cached.is_file() else minimal(evidence)
            cache = Path(os.environ.get("ENGINE_SIM_PROFILE_CACHE", args.deps_root / "cache"))
            evidence.converter(cache)
            out = work / "mixed"
            receipt = evidence.bootstrap(args.source, out, "runtime",
                        ("--with-desktop", "--with-capture", "--with-converter", "--tests"), cache)
            assert set(receipt["archives"]) == {"boost", "googletest", "sdl3"}, receipt
            assert set(receipt["git"]) == {SOLVER, "piranha"}, receipt
            actual = {variant: evidence.sdk(args.source, out, variant, mixed=True)
                      for variant in ("static", "shared")}
            write_json(work / "summary.json", actual)
            for variant, expected in baseline.items():
                differing = sorted(key for key, value in expected.items() if value != actual[variant][key])
                assert not differing, (variant, differing, work)
        print(f"{args.mode} profile gate passed")


if __name__ == "__main__":
    main()
