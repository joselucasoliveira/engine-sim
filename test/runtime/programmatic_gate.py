"""Build independent consumers of the optional installed C++ configuration SDK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


SAMPLE = Path("examples/runtime/programmatic")
IDS_DIAGNOSTIC = "CFG_REF /components/banks/0/cylinders/0/rod_journal"
CONFIG_INPUTS = ("cmake/ConfigSdk.cmake", "cmake/EngineSimConfigConfig.cmake.in", "LICENSE")
FORBIDDEN = ("nlohmann", "simple-2d-constraint-solver", "scripting", "piranha", "csv-io", "SDL", "delta-studio")


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def matching_graph(authored, frozen):
    a, b = read_json(authored), read_json(frozen)
    ids = {a["engine"]["id"]: b["engine"]["id"]}
    assert set(a["components"]) == set(b["components"])
    for name, definitions in a["components"].items():
        match = b["components"][name]
        assert len(definitions) == len(match)
        ids.update((node["id"], counterpart["id"]) for node, counterpart in zip(definitions, match))
    assert len(set(ids.values())) == len(ids)

    def convert(value):
        if isinstance(value, dict):
            return {key: convert(item) for key, item in value.items()}
        if isinstance(value, list):
            return [convert(item) for item in value]
        return ids.get(value, value) if isinstance(value, str) else value

    assert convert(a) == b
    return len(ids)


def inside(mode, work):
    commands = []
    cwd = work / "unrelated"
    cwd.mkdir()
    assert all(not Path(path).exists() for path in ("/home", "/run", "/dev/dri", "/dev/snd"))
    assert not (work / "producer").exists()

    def run(name, *arguments, expected=None):
        argv = list(map(str, arguments))
        result = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        output = result.stdout.decode(errors="replace")
        (work / f"{name}.log").write_bytes(result.stdout)
        commands.append({"argv": argv, "cwd": str(cwd), "exit": result.returncode, "log": f"{name}.log"})
        write_json(work / "commands.json", commands)
        if expected is None:
            assert result.returncode == 0, (name, output)
        else:
            assert result.returncode != 0 and expected in output, (name, output)
        return output

    def build(name, source, prefix, *options):
        binary = work / (name + "-build")
        run(name + "-configure", "cmake", "-S", source, "-B", binary, f"-DCMAKE_PREFIX_PATH={prefix}",
            "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF", "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF", *options)
        run(name + "-build", "cmake", "--build", binary, "--config", "Release", "--parallel", "3", "--verbose")
        entries = read_json(binary / "compile_commands.json")
        assert entries
        for item in entries:
            text = item["command"]
            assert str(prefix / "include") in text and not any(path in text for path in FORBIDDEN), text
            assert "/home/" not in text, text
        return binary

    def link(build_dir, target):
        line = (build_dir / "CMakeFiles" / (target + ".dir") / "link.txt").read_text()
        return shlex.split(line)

    sample = work / "example-source"
    resources = work / "Resources Ω"
    original = resources / "main.json"
    expansion_prefix = work / ("Json SDK Ω" if mode == "model" else "Runtime static Ω")
    writer = build("authored-expansion", work / "authored-expansion-source", expansion_prefix)
    expanded = work / "effective-v1.json"
    run("expand-authored-model", writer / "expand-authored", "--expand", original, expanded)
    assert sha(expanded) == "b0d67dae7bab867eed3f79fd9aae581b3eabaac329e0a769c1f82a3408e12bfe"
    if mode == "model":
        model_prefix = work / "Model SDK Ω"
        header_source = work / "header-source"
        headers = build("public-headers", header_source, model_prefix)
        for name in ("value-header", "validation-header"):
            run(name, headers / name)
        run("reject-missing-Json", "cmake", "-S", sample, "-B", work / "require-json",
            f"-DCMAKE_PREFIX_PATH={model_prefix}", "-DENGINE_SIM_PROGRAMMATIC_RUNTIME=OFF", expected="EngineSimConfig_FOUND to FALSE")
        json_prefix = work / "Json SDK Ω"
        for prefix in (model_prefix, json_prefix):
            assert not list(prefix.rglob("runtime.h")) and not list(prefix.rglob("*libengine-sim-runtime.*"))
            assert not list(prefix.rglob("nlohmann")) and not list(prefix.rglob("*solver*"))
        assert not list(model_prefix.rglob("config-json.hpp"))
        assert len(list(json_prefix.rglob("config-json.hpp"))) == 1
        assert len(list(json_prefix.rglob("nlohmann-json-MIT.txt"))) == 1

        json_binary = build("config-json", sample, json_prefix, "-DENGINE_SIM_PROGRAMMATIC_RUNTIME=OFF")
        for name in ("first", "second"):
            output = run(name + "-model", json_binary / "engine-sim-model", work / (name + ".json"))
            assert IDS_DIAGNOSTIC in output
        assert sha(work / "first.json") == sha(work / "second.json")
        model_link = link(json_binary, "engine-sim-model")
        assert not any("libengine-sim-runtime" in Path(item).name for item in model_link)
        for archive in ("libengine-sim-json.a", "libengine-sim-config.a"):
            assert sum(item.endswith(archive) for item in model_link) == 1
        report = {"canonical_sha256": sha(work / "first.json"),
                  "graph_nodes": matching_graph(work / "first.json", expanded)}
    else:
        comparisons = []
        for variant in ("static", "shared"):
            prefix = work / f"Runtime {variant} Ω"
            binary = build(variant, sample, prefix)
            for archive in ("libengine-sim-json.a", "libengine-sim-config.a"):
                assert sum(item.endswith(archive) for item in link(binary, "engine-sim-programmatic")) == 1
                assert sum(item.endswith(archive) for item in link(binary, "engine-sim-file-loader")) == (variant == "static")
            for rate in (44100, 48000):
                for mode_name in ("free", "driven"):
                    stem = f"{variant}-{rate}-{mode_name}"
                    generated = work / (stem + ".json")
                    typed, file = work / (stem + "-typed.f32"), work / (stem + "-file.f32")
                    output = run(stem + "-typed", binary / "engine-sim-programmatic", resources, rate, mode_name, generated, typed)
                    assert IDS_DIAGNOSTIC in output
                    first = next(line for line in output.splitlines() if line.startswith("{"))
                    second = run(stem + "-file", binary / "engine-sim-file-loader", original, resources, rate, mode_name, file)
                    assert json.loads(first) == json.loads(second)
                    assert typed.stat().st_size == rate * 8 and sha(typed) == sha(file)
                    comparisons.append({"variant": variant, "rate": rate, "mode": mode_name, "pcm_sha256": sha(file),
                                        "model_sha256": sha(generated), "nodes": matching_graph(generated, expanded),
                                        "telemetry": json.loads(second)})
            reverse = build(variant + "-runtime-first", work / "reverse-source", prefix)
            for archive in ("libengine-sim-json.a", "libengine-sim-config.a"):
                assert sum(item.endswith(archive) for item in link(reverse / "example", "engine-sim-programmatic")) == 1
            run(variant + "-reverse-program", reverse / "example/engine-sim-programmatic", resources,
                44100, "free", work / (variant + "-reverse.json"), work / (variant + "-reverse.f32"))
            assert sha(work / (variant + "-reverse.f32")) == sha(work / (variant + "-44100-free-file.f32"))
            input_sha = sha(original)
            assert "CFG_RESOURCE_BYTES" in run(variant + "-reset-exact-bytes", reverse / "reset-input", original, resources)
            assert sha(original) == input_sha
        for left, right in zip(comparisons[:4], comparisons[4:]):
            assert {k: v for k, v in left.items() if k != "variant"} == {k: v for k, v in right.items() if k != "variant"}
        report = {"comparisons": comparisons}
    write_json(work / "result.json", report)
    print("Installed programmatic SDK consumer verified")


def project(audit, mode, root):
    from profile_gate import JSON_FILES, SOLVER
    args = audit.args
    payload = root / "consumer"
    payload.mkdir()
    for name in ("main.json", "impulse.wav"):
        directory = payload / "Resources Ω"
        directory.mkdir(exist_ok=True)
        shutil.copyfile(args.source / "examples/runtime/fixture" / name, directory / name)
    shutil.copytree(args.source / SAMPLE, payload / "example-source")
    shutil.copyfile(__file__, payload / "gate.py")
    expander = payload / "authored-expansion-source"
    expander.mkdir()
    shutil.copyfile(args.source / "examples/config/roundtrip.cpp", expander / "main.cpp")
    (expander / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.25)\nproject(AuthoredExpansion LANGUAGES CXX)\n'
        'find_package(EngineSimConfig 1 CONFIG REQUIRED COMPONENTS Json)\n'
        'add_executable(expand-authored main.cpp)\n'
        'target_link_libraries(expand-authored PRIVATE EngineSim::Json)\n', encoding="utf-8")

    def install(build, prefix, component):
        audit.run("install-" + component, [args.cmake, "--install", build, "--config", "Release",
                                          "--prefix", prefix, "--component", component])

    if mode == "model":
        for name, sources, options in (("Model", (), ["-DENGINE_SIM_CONFIG_JSON=OFF"]),
                                       ("Json", JSON_FILES, [])):
            source = audit.projection(name.lower(), (*CONFIG_INPUTS, "modules/config", *sources))
            out = source.parent / "out"
            receipt = audit.bootstrap(source, out, "config")
            assert not receipt["git"] and not receipt["archives"]
            build = audit.configure(source, out, "config", "-DENGINE_SIM_INSTALL_CONFIG_SDK=ON", *options)
            audit.build(build)
            prefix = out / "install"
            install(build, prefix, "ConfigSDK")
            shutil.move(prefix, payload / f"{name} SDK Ω")
        headers = payload / "header-source"
        headers.mkdir()
        shutil.copyfile(args.source / "test/config/public_header.cpp", headers / "value.cpp")
        (headers / "validation.cpp").write_text(
            '#include <engine_sim/config-validation.hpp>\n'
            'int main() { namespace c = engine_sim::config_v1;\n'
            '    return std::holds_alternative<c::Diagnostic>(c::normalize(c::Document{})) ? 0 : 1;\n'
            '}\n')
        (headers / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.25)\nproject(ModelHeaders LANGUAGES CXX)\n'
            'set(CMAKE_CXX_STANDARD 17)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\nset(CMAKE_CXX_EXTENSIONS OFF)\n'
            'find_package(EngineSimConfig 1 CONFIG REQUIRED COMPONENTS Config)\n'
            'add_executable(value-header value.cpp)\nadd_executable(validation-header validation.cpp)\n'
            'target_link_libraries(value-header PRIVATE EngineSim::Config)\n'
            'target_link_libraries(validation-header PRIVATE EngineSim::Config)\n')
    else:
        source = audit.sdk_source()
        for path in CONFIG_INPUTS[:2]:
            shutil.copyfile(args.source / path, source / path)
        out = source.parent / "out"
        receipt = audit.bootstrap(source, out, "runtime")
        assert set(receipt["git"]) == {SOLVER} and not receipt["archives"]
        for variant in ("static", "shared"):
            build = audit.configure(source, out, f"runtime-{variant}", "-DENGINE_SIM_INSTALL_CONFIG_SDK=ON")
            audit.build(build, "engine-sim-runtime")
            prefix = build / "install"
            install(build, prefix, "RuntimeSDK")
            assert sorted(file.name for file in (prefix / "include/engine_sim").iterdir()) == ["runtime.h"]
            archives = {p: sha(p) for p in prefix.rglob("*.a")}
            install(build, prefix, "ConfigSDK")
            assert all(sha(path) == digest for path, digest in archives.items())
            shutil.move(prefix, payload / f"Runtime {variant} Ω")
        reverse = payload / "reverse-source"
        reverse.mkdir()
        shutil.copytree(args.source / SAMPLE, reverse / "example")
        shutil.copyfile(args.source / "test/runtime/programmatic_reset.cpp", reverse / "reset.cpp")
        (reverse / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.25)\nproject(RuntimeFirst LANGUAGES C CXX)\n'
            'find_package(EngineSimRuntime 2 CONFIG REQUIRED)\n'
            'add_executable(reset-input reset.cpp)\ntarget_link_libraries(reset-input PRIVATE EngineSim::Runtime)\n'
            'add_subdirectory(example)\n')

    command = read_json_prefix()
    command += ["--unshare-all", "--die-with-parent", "--new-session", "--clearenv"]
    for path in ("/usr", "/lib", "/lib64", "/bin", "/etc"):
        if Path(path).exists():
            command += ["--ro-bind", path, path]
    command += ["--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp", "--bind", payload, "/work",
                "--chdir", "/work", "--setenv", "PATH", "/usr/bin:/bin", "--setenv", "LC_ALL", "C.UTF-8",
                "/usr/bin/python3", "-B", "/work/gate.py", mode, "--inside"]
    audit.run("source-hidden-consumer", command)


def read_json_prefix():
    value = json.loads(os.environ.get("ENGINE_SIM_BWRAP_COMMAND", '["bwrap"]'))
    assert isinstance(value, list) and value and all(isinstance(item, str) and item for item in value)
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("model", "runtime"))
    parser.add_argument("--source", type=Path)
    parser.add_argument("--deps-root", type=Path)
    parser.add_argument("--cmake")
    parser.add_argument("--compiler")
    parser.add_argument("--jobs", default=os.environ.get("ENGINE_SIM_PROFILE_JOBS", "3"))
    parser.add_argument("--inside", action="store_true")
    args = parser.parse_args()
    if args.inside:
        inside(args.mode, Path("/work"))
        return

    from profile_gate import Audit
    if output := os.environ.get("ENGINE_SIM_GATE_OUTPUT"):
        root = Path(output) / ("installed-config-" + args.mode)
        root.mkdir()
        project(Audit(args, root), args.mode, root)
    else:
        with tempfile.TemporaryDirectory(prefix="installed-config-", dir=args.deps_root) as temporary:
            root = Path(temporary)
            project(Audit(args, root), args.mode, root)


if __name__ == "__main__":
    main()
