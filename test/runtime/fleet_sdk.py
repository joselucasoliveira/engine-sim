"""Build, relocate and consume FleetSDK artifacts using only transferred inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

FLEET_EXPORTS = {
    "es_rt_api_version", "es_rt_create", "es_rt_prepare_file", "es_rt_prepare_bytes", "es_rt_discard",
    "es_rt_submit", "es_rt_poll_result", "es_rt_poll_snapshot", "es_rt_get_transport", "es_rt_start",
    "es_rt_process", "es_rt_quiesce", "es_rt_collect", "es_rt_destroy",
}
V2_EXPORTS = {
    "es_api_version", "es_capabilities", "es_config_init", "es_create", "es_load_json_file",
    "es_load_json_bytes", "es_set_controls", "es_render", "es_get_telemetry", "es_reset",
    "es_reset_detailed", "es_unload", "es_destroy",
}
LICENSES = {"EngineSim-MIT.txt", "THIRD_PARTY_NOTICES.md", "nlohmann-json-MIT.txt", "Solver-MIT.txt"}
PRIVATE = {"engine-sim-runtime-json-private", "engine-sim-runtime-backend", "engine-sim-assembly",
           "engine-sim-json", "engine-sim-config", "engine-sim", "simple-2d-constraint-solver"}


def need(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(root):
    return {p.relative_to(root).as_posix(): sha(p) for p in sorted(root.rglob("*")) if p.is_file()}


def run(argv, cwd, log, *, rejected=None, env=None):
    args = [str(x) for x in argv]
    with log.open("a", encoding="utf-8") as f:
        f.write(f"$ {shlex.join(args)}\n")
        f.flush()
        result = subprocess.run(args, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, errors="replace", timeout=420)
        f.write(result.stdout)
        f.write(f"exit {result.returncode}\n")
    if rejected is None:
        need(result.returncode == 0, f"{shlex.join(args)} failed; see {log}")
    else:
        need(result.returncode != 0 and rejected in result.stdout, f"missing {rejected} negative control; see {log}")
    return result.stdout


def closure(build):
    reply = build / ".cmake/api/v1/reply"
    index = sorted(reply.glob("index-*.json"))[-1]
    model = json.loads(index.read_text())["reply"]["codemodel-v2"]["jsonFile"]
    rows = json.loads((reply / model).read_text())["configurations"][0]["targets"]
    by_id = {row["id"]: row for row in rows}
    queue = [next(row["id"] for row in rows if row["name"] == "engine-sim-fleet")]
    edges = {}
    while queue:
        row = by_id[queue.pop()]
        if row["name"] in edges:
            continue
        target = json.loads((reply / row["jsonFile"]).read_text())
        links = [dependency["id"] for dependency in target.get("dependencies", [])]
        edges[row["name"]] = sorted(by_id[dep]["name"] for dep in links)
        queue.extend(links)
    need(PRIVATE <= edges.keys(), "Fleet closure missing its locked private dependencies")
    need(not any(re.search(r"piranha|portaudio|delta|sdl|convert|desktop", name, re.I) for name in edges),
         "unwanted runtime artifact in Fleet link closure")
    return edges


def audit_code(code, variant, source):
    files = inventory(code)
    by_name = {Path(name).name for name in files}
    need({"fleet.h", "EngineSimFleetConfig.cmake", "EngineSimFleetConfigVersion.cmake"} | LICENSES <= by_name,
         "incomplete code-only Fleet paths")
    need(all(not any(piece in name for piece in ("example/", "PortAudio", "asset", "solver-overlay"))
             and not name.endswith((".json", ".wav", ".cpp", ".c")) for name in files), "data in code-only Fleet SDK")
    need(len([name for name in files if name.endswith(".h")]) == 1, "private headers escaped")
    licenses = code / next(name for name in files if name.endswith("licenses/EngineSim-MIT.txt")).rsplit("/", 1)[0]
    for leaf, input_path in (
        ("EngineSim-MIT.txt", "LICENSE"), ("THIRD_PARTY_NOTICES.md", "cmake/fleet-sdk-notices.md"),
        ("nlohmann-json-MIT.txt", "modules/json/third_party/nlohmann/LICENSE.MIT"),
        ("Solver-MIT.txt", "dependencies/submodules/simple-2d-constraint-solver/LICENSE"),
    ):
        need(sha(licenses / leaf) == sha(source / input_path), f"invalid notice {leaf}")
    for name in files:
        path = code / name
        need(path.resolve().is_relative_to(code), f"file escaped code prefix: {name}")
        if name.endswith(".cmake"):
            metadata = path.read_text()
            need(str(source) not in metadata and str(code) not in metadata and "/home/" not in metadata,
                 f"build path exported in {name}")
        if name.endswith((".a", ".lib")):
            need(path.open("rb").read(8) != b"!<thin>\n", f"thin archive {name}")
    private = [name for name in files if "/engine-sim-fleet/" in name and name.endswith((".a", ".lib"))]
    private_names = {Path(name).stem.removeprefix("lib") for name in private}
    need(private_names == (PRIVATE if variant == "static" else set()), "non-closed static SDK archive inventory")
    metadata = {"fleet.h", "EngineSimFleetConfig.cmake", "EngineSimFleetConfigVersion.cmake"} | LICENSES
    extra = by_name - metadata - {Path(name).name for name in private}
    need(extra and all(re.fullmatch(r"(?:lib)?engine-sim-fleet(?:-1|\.1(?:\.0\.0)?)?\.(?:a|lib|dll|dylib|so(?:\.1(?:\.0\.0)?)?)", name)
                       for name in extra), f"unknown Fleet SDK outputs: {extra}")
    return files


def produce(args, work, log):
    build = args.build.resolve()
    (build / ".cmake/api/v1/query").mkdir(parents=True, exist_ok=True)
    (build / ".cmake/api/v1/query/codemodel-v2").touch()
    source = args.source.resolve()
    options = [
        "-DBUILD_TESTING=OFF", "-DCMAKE_BUILD_TYPE=Release", "-DENGINE_SIM_BUILD_RUNTIME=ON",
        f"-DENGINE_SIM_RUNTIME_SHARED={'ON' if args.variant == 'shared' else 'OFF'}",
        "-DENGINE_SIM_INSTALL_RUNTIME_EXAMPLE=OFF", "-DENGINE_SIM_INSTALL_FLEET_EXAMPLE=ON",
        "-DENGINE_SIM_BUILD_RUNTIME_EXAMPLES=OFF", "-DENGINE_SIM_INSTALL_CONFIG_SDK=OFF",
    ]
    mode = "ON" if args.combined_deps else "OFF"
    options += [f"-DENGINE_SIM_BUILD_{name}={mode}" for name in ("DESKTOP", "CAPTURE", "LEGACY_CONVERTER")]
    if args.combined_deps:
        options.append(f"-DENGINE_SIM_DEPS_FILE={args.combined_deps.resolve()}")
    run(["cmake", "-S", source, "-B", build, *options], work, log)
    run(["cmake", "--build", build, "--config", "Release", "--parallel", "8", "--target",
         "engine-sim-fleet", "engine-sim-runtime"], work, log)
    code = work / "original prefix"
    examples = work / "example component"
    for component, destination in (("FleetSDK", code), ("FleetSDKExamples", examples)):
        run(["cmake", "--install", build, "--config", "Release", "--prefix", destination, "--component", component],
            work, log)
    code_files = audit_code(code, args.variant, source)
    sample_dir = next(examples.rglob("fixture-single")).parent
    example_files = inventory(sample_dir)
    manifest = json.loads((source / "cmake/fleet-example.json").read_text())["files"]
    need(example_files == {item["path"]: item["sha256"] for item in manifest},
         "example installation differs from the digest allowlist")
    need(len(inventory(examples)) == len(example_files), "extra files in example component")
    run(["cmake", "--install", build, "--config", "Release", "--prefix", code, "--component", "RuntimeSDK"], work, log)
    moved = work / "SDK espaço Ω"
    code.rename(moved)
    need(not code.exists(), "the original SDK prefix still resolves")
    shutil.copytree(sample_dir, work / "examples")
    shutil.copytree(source / "test/runtime/fleet-sdk", work / "consumer")
    shutil.copy2(__file__, work / "fleet_sdk.py")
    data = {"variant": args.variant, "closure": closure(build), "code_only": code_files,
            "examples": example_files, "payload": inventory(moved)}
    (work / "transfer.json").write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return data


def gear(executable, build):
    suffix = ".exe" if os.name == "nt" else ""
    choices = [p for p in build.rglob(executable + suffix) if p.is_file()]
    need(len(choices) == 1, f"expected one executable named {executable}")
    return choices[0]


def symbols(lib, names, work, log):
    text = run(["nm", "-D", "--defined-only", lib], work, log)
    need({line.split()[-1].split("@")[0] for line in text.splitlines() if line.strip()} == names,
         f"incorrect dynamic allowlist: {lib}")
    dynamic = run(["readelf", "-dW", lib], work, log)
    need(not re.search(r"piranha|delta|sdl|portaudio|engine-sim-(runtime|fleet)-", dynamic, re.I),
         "optional libraries in shared deployment")
    return dynamic


def consume(work, variant, log):
    moved = work / "SDK espaço Ω"
    examples = work / "examples"
    transfer = json.loads((work / "transfer.json").read_text(encoding="utf-8"))
    need(transfer["variant"] == variant and inventory(moved) == transfer["payload"] and
         inventory(examples) == transfer["examples"], "consumer payload changed in transfer")
    if platform.system() == "Linux":
        need(not Path("/home").exists() and not Path("/run").exists() and not Path("/dev/snd").exists(),
             "consumer needs the source-hidden Linux namespace")
    dual = work / "dual-build"
    example_build = work / "hosts-build"
    prefix = f"-DCMAKE_PREFIX_PATH={moved}"
    build_flags = [prefix, "-DCMAKE_BUILD_TYPE=Release"]
    for src, build in ((work / "consumer", dual), (examples, example_build)):
        run(["cmake", "-S", src, "-B", build, *build_flags,
             f"-DFLEET_SHARED_WITNESS={'ON' if src.name == 'consumer' and variant == 'shared' else 'OFF'}"],
            work, log)
        run(["cmake", "--build", build, "--config", "Release", "--parallel", "5"], work, log)
    env = os.environ.copy()
    if os.name == "nt":
        dlls = [str(file.parent) for file in moved.rglob("*.dll")]
        env["PATH"] = os.pathsep.join([*dlls, env.get("PATH", "")])
    run([gear("fleet-dual", dual)], work, log, env=env)
    run([sys.executable, "-B", examples / "verify.py", gear("engine-sim-fleet-c-host", example_build),
         gear("engine-sim-fleet-cxx-host", example_build), examples / "fixture-single/main.json",
         examples / "fixture-heavy/main.json"], work, log, env=env)

    if platform.system() == "Linux" and variant == "shared":
        fleet = next(moved.rglob("libengine-sim-fleet.so"))
        runtime = next(moved.rglob("libengine-sim-runtime.so"))
        dynamic = symbols(fleet, FLEET_EXPORTS, work, log)
        symbols(runtime, V2_EXPORTS, work, log)
        need("libengine-sim-fleet.so.1" in dynamic and "$ORIGIN" in dynamic and "BIND_NOW" in dynamic,
             "shared Fleet library loader identity")
        dependencies = run(["ldd", gear("fleet-dual", dual)], work, log)
        need(str(fleet.parent) in dependencies and "libengine-sim-fleet.so.1" in dependencies and
             "libengine-sim-runtime.so.2" in dependencies and "not found" not in dependencies,
             "wrong relocated runtime loader paths")
        env["LD_PRELOAD"] = str(next(dual.rglob("libfleet-probe-heap.so")))
        probe = run([gear("fleet-shared-probe", dual)], work, log, env=env)
        need("cold=0 injected=2" in probe, "installed shared heap negative control was not observed")
    if platform.system() == "Linux":
        shutil.copy2(dual / "fleet-dual.map", work / "link.map")
    wrong = work / "wrong-major"
    wrong.mkdir()
    (wrong / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.25)\nproject(Wrong LANGUAGES C CXX)\n"
        "find_package(EngineSimFleet 2.0 CONFIG REQUIRED)\n", encoding="utf-8")
    run(["cmake", "-S", wrong, "-B", work / "wrong-build", prefix], work, log, rejected="compatible")
    if variant == "static":
        damaged = work / "missing-private"
        shutil.copytree(moved, damaged, symlinks=True)
        archive = next(p for p in damaged.rglob("*simple-2d-constraint-solver*") if
                       p.suffix in (".a", ".lib") and p.parent.name == "engine-sim-fleet")
        archive.unlink()
        run(["cmake", "-S", examples, "-B", work / "missing-build", f"-DCMAKE_PREFIX_PATH={damaged}"],
            work, log, rejected="Missing or invalid EngineSim Fleet SDK file")
    print("source-hidden" if platform.system() == "Linux" else "consumer payload")
    print(f"verified FleetSDK variant={variant} platform={platform.system()} arch={platform.machine()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", choices=["static", "shared"], required=True)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--combined-deps", type=Path)
    parser.add_argument("--consume", type=Path)
    parser.add_argument("--transfer", type=Path, help="Prepare declared consumer payload for another native host")
    args = parser.parse_args()
    if args.consume:
        consume(args.consume.resolve(), args.variant, args.consume / "consumer.log")
        return
    need(args.build, "producer needs a build directory")
    args.source = args.source or Path(__file__).resolve().parents[2]
    target = Path(os.environ["ES_FLEET_EVIDENCE"]).resolve() if "ES_FLEET_EVIDENCE" in os.environ else None
    if args.transfer:
        work = args.transfer.resolve()
        work.mkdir(parents=True, exist_ok=False)
        produce(args, work, work / "producer.log")
        return
    with tempfile.TemporaryDirectory(prefix="fleet-sdk-") as temp:
        work = Path(temp)
        log = work / "producer.log"
        try:
            produce(args, work, log)
            if platform.system() == "Linux":
                sandbox = ["bwrap", "--unshare-all", "--die-with-parent", "--new-session"]
                for directory in ("/usr", "/lib", "/lib64", "/bin", "/etc"):
                    if Path(directory).exists():
                        sandbox += ["--ro-bind", directory, directory]
                sandbox += ["--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp", "--bind", work, "/work",
                            "--chdir", "/work", "--clearenv", "--setenv", "PATH", "/usr/bin:/bin",
                            "--setenv", "LC_ALL", "C.UTF-8", "/usr/bin/python3", "-B", "/work/fleet_sdk.py",
                            "--variant", args.variant, "--consume", "/work"]
                print(run(sandbox, work, log), end="")
            else:
                consume(work, args.variant, work / "consumer.log")
        finally:
            if target:
                label = args.variant + ("-combined" if args.combined_deps else "")
                output = target / ("installed-" + label)
                output.mkdir(exist_ok=False, parents=True)
                for name in ("producer.log", "consumer.log", "transfer.json", "link.map"):
                    if (work / name).is_file():
                        shutil.copy2(work / name, output / name)
            else:
                for name in ("producer.log", "consumer.log"):
                    if (work / name).is_file():
                        print((work / name).read_text(), end="")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
