"""Run the bounded preparation and installed-cost CTest workloads."""
import argparse
import csv
import hashlib
import itertools
import os
from pathlib import Path
import shutil
import sys
import tempfile

from profile_gate import Audit, OPTIONAL_INPUT, SOLVER, read_json, write_json


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def phases(audit, args):
    summaries = {}
    for rate, mode in itertools.product((44100, 48000), ("free", "driven")):
        outputs = []
        for kind in ("file", "bytes"):
            name = f"{rate}-{mode}-{kind}"
            output = audit.root / f"{name}.csv"
            audit.run(name, [args.executable, args.fixture / "main.json", args.fixture,
                             output, kind, rate, mode, args.cycles], cwd=audit.root)
            with output.open(newline="", encoding="utf-8") as file:
                rows = list(csv.DictReader(file))
            by_key = {(row["operation"], row["phase"]): row for row in rows}
            required = ("tokenize_parse", "schema", "normalization", "semantic_graph",
                        "resource_io", "resource_decode", "geometry_solver", "dsp", "convolution", "activation")
            for op in ("cold", "warm_replacement", "reset", "two_owners"):
                assert len(by_key) == len(rows)
                assert all(int(by_key[op, phase]["wall_ns"]) > 0 for phase in required)
            for cycle in (10, 50, 100):
                if cycle <= args.cycles:
                    shared = [by_key[f"cycle_{cycle}", stage] for stage in ("shared_schema", "shared_filter")]
                    assert all(int(row["live_bytes"]) > 0 for row in shared)
                    assert int(shared[0]["total_live_bytes"]) == sum(int(row["live_bytes"]) for row in shared)
            assert by_key["steady", "render"]["allocations"] == "0"
            raw = Path(str(output) + ".f32le")
            assert raw.stat().st_size == 8192 * 4
            outputs.append(digest(raw))
            summaries[name] = {"report": output.name, "sha256": digest(output), "pcm_sha256": outputs[-1]}
        assert outputs[0] == outputs[1], (rate, mode, outputs)
    write_json(audit.root / "result.json", {"status": "pass", "cases": summaries,
               "executable_sha256": digest(args.executable), "fixture_sha256": digest(args.fixture / "main.json"),
               "overlay_sha256": digest(args.overlay), "cycles_per_process": args.cycles})


def producer(audit, args, variant, source, out):
    if sys.platform.startswith("linux"):
        options = ()
        if variant == "shared":
            options = (f"-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-Map,{out / 'runtime-shared/sdk-link.map'}",)
        build = audit.configure(source, out, f"runtime-{variant}", *options)
        audit.build(build, "engine-sim-runtime")
        return build

    build = out / f"runtime-{variant}"
    env = dict(audit.env, ES_OUT=str(args.deps_root))
    audit.run(f"configure-{variant}", [args.cmake, "-S", source, "-B", build, "-G", args.generator,
              *(["-A", args.platform] if args.platform else []), "-DCMAKE_BUILD_TYPE=Release",
              f"-DCMAKE_CXX_COMPILER={args.compiler}", f"-DENGINE_SIM_DEPS_FILE={args.deps_root / 'deps.cmake'}",
              "-DENGINE_SIM_BUILD_RUNTIME=ON", f"-DENGINE_SIM_RUNTIME_SHARED={'ON' if variant == 'shared' else 'OFF'}",
              "-DENGINE_SIM_BUILD_DESKTOP=OFF", "-DENGINE_SIM_BUILD_HOST_TESTS=OFF",
              "-DENGINE_SIM_BUILD_CAPTURE=OFF", "-DENGINE_SIM_BUILD_LEGACY_CONVERTER=OFF", "-DBUILD_TESTING=OFF"],
              env=env)
    audit.build(build, "engine-sim-runtime")
    return build


def installed(audit, args):
    if sys.platform.startswith("linux"):
        source = audit.sdk_source()
        out = source.parent / "output"
        receipt = audit.bootstrap(source, out, "runtime")
        assert set(receipt["git"]) == {SOLVER} and not receipt["archives"]
    else:
        source, out = args.source, audit.root / "output"

    project = audit.root / "independent-consumer"
    shutil.copytree(args.source / "test/runtime/cost-consumer", project)
    results = {}
    for variant in ("static", "shared"):
        build = producer(audit, args, variant, source, out)
        stage, prefix = audit.root / f"stage-{variant}", audit.root / f"Runtime {variant} Ω"
        audit.run(f"install-{variant}", [args.cmake, "--install", build, "--config", "Release",
                  "--prefix", stage, "--component", "RuntimeSDK"])
        stage.rename(prefix)
        assert not any(path.suffix in (".mr", ".wav") for path in prefix.rglob("*"))
        closure = audit.inspect_sdk(source, build, prefix, variant) if sys.platform.startswith("linux") else None
        output = audit.root / f"consumer-{variant}"
        producer_link_map = [f"-DCMAKE_EXE_LINKER_FLAGS=-Wl,-Map,{output / 'runtime-cost.map'}"] if sys.platform.startswith("linux") else []
        audit.run(f"cost-configure-{variant}", [args.cmake, "-S", project, "-B", output, "-G", args.generator,
                  *(["-A", args.platform] if args.platform else []), "-DCMAKE_BUILD_TYPE=Release",
                  f"-DCMAKE_CXX_COMPILER={args.compiler}", f"-DCMAKE_PREFIX_PATH={prefix}",
                  "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF", "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF",
                  *producer_link_map], cwd=audit.root)
        audit.run(f"cost-build-{variant}", [args.cmake, "--build", output, "--config", "Release",
                  "--target", "runtime-cost", "--parallel", args.jobs, "--verbose"], cwd=audit.root)
        matches = [file for file in output.rglob("runtime-cost.exe" if os.name == "nt" else "runtime-cost")
                   if file.is_file()]
        assert len(matches) == 1, matches
        exe = matches[0]
        env = dict(audit.env, PATH=str(prefix / "bin") + os.pathsep + audit.env.get("PATH", ""))
        if sys.platform.startswith("linux"):
            images = audit.run(f"cost-images-{variant}", ["ldd", exe], cwd=audit.root)
            assert not OPTIONAL_INPUT.search(images), images
            if variant == "shared":
                assert str(prefix / "lib") in images, images
            else:
                assert "libengine-sim-runtime" not in images, images
        elif sys.platform == "darwin":
            images = audit.run(f"cost-images-{variant}", ["otool", "-L", exe], cwd=audit.root)
            assert not OPTIONAL_INPUT.search(images), images
        elif shutil.which("dumpbin"):
            images = audit.run(f"cost-images-{variant}", ["dumpbin", "/DEPENDENTS", exe], cwd=audit.root)
            assert not OPTIONAL_INPUT.search(images), images

        study = audit.root / f"study-{variant}"
        audit.run(f"cost-smoke-{variant}", [sys.executable, "-B", args.source / "tools/runtime_bench.py",
                  "installed-costs", "--installed", exe, "--fixture", args.fixture, "--out", study,
                  "--smoke"], cwd=audit.root, env=env)
        report = read_json(study / "run.json")
        assert report["status"] == "pass" and len(report["cases"]) == 4
        outputs = {}
        for case in report["cases"]:
            assert len(case["rows"]) == 4
            outputs[f"{case['rate']}-{case['mode']}"] = case["rows"][0]["owners"]
        files = {item.relative_to(prefix).as_posix(): {"bytes": item.stat().st_size, "sha256": digest(item)}
                 for item in sorted(prefix.rglob("*")) if item.is_file()}
        results[variant] = {"report": str(study / "run.json"), "closure": closure, "outputs": outputs,
                            "files": files, "executable_bytes": exe.stat().st_size, "executable_sha256": digest(exe)}
    assert results["static"]["outputs"] == results["shared"]["outputs"]
    write_json(audit.root / "result.json", {"status": "pass", "profiles": results})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("phases", "installed"))
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--deps-root", type=Path, required=True)
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--overlay", type=Path)
    parser.add_argument("--cycles", type=int, choices=(10, 100), default=10)
    parser.add_argument("--compiler")
    parser.add_argument("--cmake")
    parser.add_argument("--ctest")
    parser.add_argument("--generator")
    parser.add_argument("--platform")
    parser.add_argument("--jobs", default=os.environ.get("ENGINE_SIM_PROFILE_JOBS", "3"))
    args = parser.parse_args()
    action = phases if args.mode == "phases" else installed
    if evidence := os.environ.get("ENGINE_SIM_GATE_OUTPUT"):
        root = Path(evidence) / f"runtime-cost-{args.mode}"
        root.mkdir(parents=True)
        action(Audit(args, root), args)
    else:
        with tempfile.TemporaryDirectory(prefix=f"runtime-cost-{args.mode}-", dir=args.deps_root) as work:
            action(Audit(args, Path(work)), args)


if __name__ == "__main__":
    main()
