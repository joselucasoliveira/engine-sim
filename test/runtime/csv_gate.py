"""Prove that the product builds without csv-io and preserves its text formats."""
import argparse
import csv
import os
from pathlib import Path
import shutil
import sys
import tempfile

from installed_consumer import file_sha256
from profile_gate import Audit, SOLVER, read_json, write_json


PROJECT_INPUTS = (
    ".gitmodules", "LICENSE", "THIRD_PARTY_NOTICES.md", "cmake", "include", "src", "modules", "schemas",
    "test", "examples", "dependencies/CMakeLists.txt", "dependencies/submodules/CMakeLists.txt",
    "tools/capture", "tools/stage_resources.py", "tools/run_desktop_smoke.py", "assets/shaders/sdl",
    "assets/engines/atg-video-1/02_kohler_ch750.json",
)
RETIRED = ("csv-io", "csv_io.h", "csv::CsvData", "atg_csv")


def without_csv(data, source):
    assert not any(token in data for token in RETIRED), f"CSV dependency found in {source}"


def check_build(audit, build, binary):
    for name in ("compile_commands.json", "configure-trace.jsonl"):
        without_csv((build / name).read_text(encoding="utf-8"), build / name)
    for query in ("commands", "deps"):
        output = audit.run(f"inputs-{build.name}-{query}", ["ninja", "-C", build, "-t", query])
        assert output
        without_csv(output, build)
    binary = build / binary
    if binary.suffix == ".a":
        output = audit.run(f"members-{build.name}", ["ar", "t", binary])
    else:
        output = audit.run(f"imports-{build.name}", ["readelf", "-d", binary])
    assert output
    without_csv(output, binary)
    without_csv(audit.run(f"symbols-{build.name}", ["nm", "-C", binary]), binary)


def check_source(source):
    assert not (source / "dependencies/submodules/csv-io").exists() and not (source / "include/csv_io.h").exists()
    for name in (".gitmodules", "cmake/dependencies.lock.json", "dependencies/submodules/CMakeLists.txt",
                 "THIRD_PARTY_NOTICES.md"):
        without_csv((source / name).read_text(encoding="utf-8"), source / name)


def capture(audit, source, out):
    receipt = audit.bootstrap(source, out, "capture")
    assert set(receipt["git"]) == {SOLVER} and not receipt["archives"]
    build = audit.configure(source, out, "capture", "-DBUILD_TESTING=OFF")
    audit.build(build, "engine-sim-capture")
    return build


def profiles(audit, source):
    args = audit.args
    cache = Path(os.environ.get("ENGINE_SIM_PROFILE_CACHE", args.deps_root / "cache"))
    core_output = audit.root / "core"
    receipt = audit.bootstrap(source, core_output, "core", ("--tests",), cache)
    assert set(receipt["git"]) == {SOLVER} and set(receipt["archives"]) == {"googletest"}
    core = audit.configure(source, core_output, "core-release")
    audit.build(core, "engine-sim-test")
    audit.run("core-regressions", [args.ctest, "--test-dir", core, "--no-tests=error", "--output-on-failure",
                                   "-R", r"^(GasSystemTests|FunctionTests|SynthesizerTests)\."])
    check_build(audit, core, "libengine-sim.a")
    capture_build = capture(audit, source, audit.root / "capture")
    check_build(audit, capture_build, "bin/engine-sim-capture")

    runtime = audit.sdk_source()
    runtime_output = runtime.parent / "out"
    receipt = audit.bootstrap(runtime, runtime_output, "runtime")
    assert set(receipt["git"]) == {SOLVER} and not receipt["archives"]
    baseline = audit.sdk(runtime, runtime_output, "static")
    mixed_output = audit.root / "mixed"
    receipt = audit.bootstrap(source, mixed_output, "runtime", ("--with-desktop", "--with-capture", "--tests"), cache)
    assert set(receipt["git"]) == {SOLVER} and set(receipt["archives"]) == {"googletest", "sdl3"}
    mixed = audit.sdk(source, mixed_output, "static", mixed=True, converter=False)
    assert baseline == mixed, sorted(key for key in baseline if baseline[key] != mixed[key])
    check_build(audit, mixed_output / "runtime-static", "bin/engine-sim-app")
    notice = mixed_output / "runtime-static/installed-consumer/SDK espaço Ω/share/engine-sim-runtime/licenses/THIRD_PARTY_NOTICES.md"
    without_csv(notice.read_text(encoding="utf-8"), notice)
    write_json(audit.root / "profiles.json", {"core": "pass", "capture": "pass", "runtime_static": "pass",
                                             "desktop_capture_tests": "pass", "relocated_sdk": baseline})


def formats(audit, source):
    args = audit.args
    build = capture(audit, source, audit.root / "capture")
    binary = build / "bin/engine-sim-capture"
    work = audit.root / "formats"
    work.mkdir()
    fixture = work / "resources"
    fixture.mkdir()
    for item in read_json(source / "cmake/runtime-example.json")["files"]:
        origin = source / "examples/runtime/fixture" / item["path"]
        assert file_sha256(origin) == item["sha256"]
        shutil.copyfile(origin, fixture / item["path"])
    runs = []
    for name in ("gas-reference", "gas-replay"):
        out = work / name
        audit.run(name, [binary, "run", "--suite", "quick", "--case", "GAS", "--out", out,
                          "--resources", fixture, "--seed", 209, "--repetitions", 1, "--mode", "sync"], cwd=work)
        run = read_json(out / "run.json")
        assert run["status"] == "pass"
        observation = run["cases"]["GAS"][0]["main"]
        trace = out / observation["directory"] / "trace.csv"
        with trace.open(encoding="utf-8", newline="") as stream:
            reader = csv.DictReader(stream)
            assert reader.fieldnames == ["step", "volume_m3", "p_pa", "t_k", "n_mol", "energy_j", "work_j"]
            rows = list(reader)
        assert [int(row["step"]) for row in rows] == list(range(0, 10001, 100))
        assert len({row["n_mol"] for row in rows}) == 1
        assert float(rows[-1]["volume_m3"]) == 0.5 * float(rows[0]["volume_m3"])
        assert observation["trace"]["rows"] == 101
        assert observation["trace"]["physical_sha256"] == observation["artifacts"]["trace.csv"] == file_sha256(trace)
        runs.append(observation)
    assert runs[0]["trace"] == runs[1]["trace"]
    sys.path.insert(0, str(source / "tools/capture"))
    from capture_cli import observation

    baseline, replay = work / "gas-reference", work / "gas-replay"
    reference = read_json(baseline / "run.json")
    policy = work / "policy.json"
    write_json(policy, {"version": 1, "baseline_sha256": file_sha256(baseline / "run.json"),
                        "tuple": reference["build"]["tuple"], "seed": reference["seed"], "mode": reference["mode"],
                        "cases": {"GAS": observation(reference["cases"]["GAS"][0])}, "intentional_allowances": []})
    compare = [binary, "compare", "--reference", baseline, "--policy", policy, "--candidate"]
    audit.run("compare-csv", [*compare, replay, "--out", work / "comparison"], cwd=work)
    assert read_json(work / "comparison/comparison.json")["status"] == "pass"

    corrupt = work / "corrupt"
    shutil.copytree(replay, corrupt)
    damaged_run = read_json(corrupt / "run.json")
    event = damaged_run["cases"]["GAS"][0]["main"]
    damaged_trace = corrupt / event["directory"] / "trace.csv"
    damaged_trace.write_bytes(damaged_trace.read_bytes() + b"10101,missing-columns\n")
    event["artifacts"]["trace.csv"] = file_sha256(damaged_trace)
    write_json(corrupt / "run.json", damaged_run)
    audit.run("reject-malformed-csv", [*compare, corrupt, "--out", work / "rejected"], cwd=work,
              expected="truncated or invalid trace row")
    assert not (work / "rejected").exists()
    audit.run("renderer-tsv", [sys.executable, "-B", args.source / "test/runtime/example_tests.py",
                              "--cli", args.render, "--reference", args.reference, "--fixture", fixture,
                              "--out", work / "renderer"], cwd=work)
    outcome, = (work / "renderer").glob("*/result.json")
    report = read_json(outcome)
    assert report["status"] == "pass" and len(report["measurements"]) == 3 and report["negative_cases"] > 0
    write_json(work / "summary.json", {"csv_trace": runs[0]["trace"], "tsv": report["measurements"],
                                      "tsv_negative_cases": report["negative_cases"]})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("profiles", "formats"))
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--deps-root", required=True, type=Path)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--ctest", required=True)
    parser.add_argument("--render", required=True, type=Path)
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--jobs", default=os.environ.get("ENGINE_SIM_PROFILE_JOBS", "3"))
    args = parser.parse_args()
    external = os.environ.get("ENGINE_SIM_GATE_OUTPUT")
    parent = Path(external) if external else args.deps_root
    with tempfile.TemporaryDirectory(prefix=f"text-{args.mode}-", dir=parent) as temporary:
        root = Path(temporary)
        if external:
            retained = parent / f"text-{args.mode}"
            root.rename(retained)
            root = retained
        audit = Audit(args, root)
        check_source(args.source)
        source = audit.projection("projected", PROJECT_INPUTS, (SOLVER,))
        check_source(source)
        (profiles if args.mode == "profiles" else formats)(audit, source)
        print(f"{args.mode} passed")


if __name__ == "__main__":
    main()
