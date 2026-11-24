"""Exercise native profile commands and independently consume real SDK transfers."""
import argparse
from copy import deepcopy
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SOURCE / "tools"))
import runtime_ci as driver


def run(argv, cwd, output, env=None, expected=0):
    result = subprocess.run([str(arg) for arg in argv], cwd=cwd, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output.write_bytes(result.stdout)
    if result.returncode != expected:
        raise RuntimeError(f"Exit {result.returncode}, expected {expected}, see {output}\n"
                           + result.stdout.decode(errors="replace")[-4000:])


def matrix(work, revision):
    for system in driver.PRODUCER_RUNNER:
        for variant in ("static", "shared"):
            overlays, flags, targets = driver.sdk_plan(variant, "minimal", system)
            assert not overlays and targets == ["engine-sim-runtime"]
            assert flags == (["-DCMAKE_OSX_ARCHITECTURES=arm64", "-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0"]
                             if system == "Darwin" else [])
            overlays, flags, targets = driver.sdk_plan(variant, "combined", system)
            assert set(overlays) == {"--with-desktop", "--with-converter", "--with-capture"} | (
                {"--tests"} if variant == "static" else set())
            assert {"-DENGINE_SIM_BUILD_DESKTOP=ON", "-DENGINE_SIM_BUILD_CAPTURE=ON",
                    "-DENGINE_SIM_BUILD_LEGACY_CONVERTER=ON"} <= set(flags)
            assert {"engine-sim-runtime", "engine-sim-app", "engine-sim-capture", "engine-sim-convert"} <= set(targets)
            assert ("engine-sim-host-test" in targets) == (variant == "static")
        assert driver.acquisition_paths("minimal", system) == driver.acquisition_paths("modules", system) == [
            "dependencies/submodules/simple-2d-constraint-solver"]
        assert driver.acquisition_paths("converter", system)[-1] == "dependencies/submodules/piranha"
        assert ("dependencies/submodules/delta-studio" in driver.acquisition_paths("combined", system)) == (system == "Windows")
    run([sys.executable, "-B", SOURCE / "tools/runtime_ci.py", "profiles", "--profile", "modules",
         "--revision", revision, "--out", work / "module-run", "--offline", "--allow-dirty"],
        SOURCE, work / "modules.log")
    proof = json.loads((work / "module-run/result.json").read_text(encoding="utf-8"))
    assert proof["status"] == "pass" and proof["native"]["system"] == platform.system()


def consume_command(payload, root, revision, variant, fleet=False):
    args = ["consume", "--variant", variant, "--revision", revision, "--allow-dirty"]
    if fleet:
        args.append("--fleet")
    if platform.system() != "Linux":
        return [sys.executable, "-B", payload / "runtime_ci.py", *args, "--relocation-only", "--payload", payload, "--out", root / "run"]
    prefix = json.loads(os.environ.get("ENGINE_SIM_BWRAP_COMMAND", '["bwrap"]'))
    command = [*prefix, "--unshare-all", "--die-with-parent", "--new-session", "--clearenv"]
    for path in ("/usr", "/lib", "/lib64", "/bin", "/etc"):
        if Path(path).exists():
            command += ["--ro-bind", path, path]
    command += ["--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp",
                "--ro-bind", payload, "/payload", "--bind", root, "/result", "--chdir", "/result",
                "--setenv", "PATH", "/usr/bin:/bin", "--setenv", "LC_ALL", "C.UTF-8"]
    for key in ("CC", "CXX"):
        if os.environ.get(key):
            command += ["--setenv", key, os.environ[key]]
    if tracer := os.environ.get("ENGINE_SIM_STRACE"):
        command += ["--ro-bind", Path(tracer).resolve(strict=True), "/strace", "--setenv", "ENGINE_SIM_STRACE", "/strace"]
    return [*command, "/usr/bin/python3", "-B", "/payload/runtime_ci.py", *args, "--payload", "/payload", "--out", "/result/run"]


def transfer(work, revision, fleet=False):
    for variant in ("static", "shared"):
        producer = work / (variant + "-producer")
        run([sys.executable, "-B", SOURCE / "tools/runtime_ci.py", "produce", "--variant", variant, "--revision", revision,
             "--out", producer, "--offline", "--allow-dirty", *(["--fleet"] if fleet else ())],
            SOURCE, work / (variant + "-producer.log"))
        payload = work / (variant + "-transfer")
        shutil.move(producer / "transfer", payload)
        manifest_path = payload / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        assert not any(name.startswith("share/engine-sim-runtime/example/") for name in manifest["sdk_files"])
        assert "fixture/main.json" in manifest["consumer_files"]
        root = work / (variant + "-consumer")
        root.mkdir()
        run(consume_command(payload, root, revision, variant, fleet), root, root / "consume.log")
        summary = json.loads((root / "run/result.json").read_text(encoding="utf-8"))
        assert summary["producer_trees_absent"] == (platform.system() == "Linux")
        assert summary["manifest_sha256"] == driver.sha256(manifest_path)
        rows = [row for row in summary["commands"] if "native_matrix" in row]
        assert len(rows) == 1 and len(rows[0]["native_matrix"]) == 2
        assert any("foreign_matrix" in row for row in summary["commands"]) == (variant == "shared")
        if fleet:
            witness = root / "run/fleet"
            assert summary["fleet"] == {
                "archive_sha256": driver.sha256(payload / "fleet.tar"),
                "installed_manifest_sha256": driver.sha256(witness / "transfer.json"),
                "consumer_log_sha256": driver.sha256(witness / "consumer.log"),
            }
            assert summary["fleet"]["archive_sha256"] == manifest["files"]["fleet.tar"]
            log = (witness / "consumer.log").read_text(encoding="utf-8")
            assert "fresh callback returned the first frame" in log and "co-installed Runtime 2 and Fleet 1" in log
            if variant == "shared" and platform.system() == "Linux":
                assert "cold=0 injected=2 release=1 aligned=1 lock=1 wait=1 throw=1" in log
        if variant == "static":
            # The same standalone transport entry point must stop before
            # compiling a client when a transferred artifact changes.
            damaged = work / "damaged-transfer"
            shutil.copytree(payload, damaged)
            altered = "fleet.tar" if fleet else "consumers.tar"
            with (damaged / altered).open("ab") as stream:
                stream.write(b"tampered")
            failure = work / "tampered-consumer"
            failure.mkdir()
            run(consume_command(damaged, failure, revision, variant, fleet), failure, failure / "consume.log", expected=1)
            rejected = json.loads((failure / "run/result.json").read_text(encoding="utf-8"))
            assert rejected["error"] == "Transfer file identity differs: " + altered and not (failure / "run/work").exists()


def fleet_matrix(work, revision):
    workflow = (SOURCE / ".github/workflows/runtime-sdk.yml").read_text(encoding="utf-8")
    for runner in ('"ubuntu-24.04"', '"windows-2022"', '"macos-15"'):
        assert workflow.count(runner) >= 2
    assert workflow.count('variant: ["static", "shared"]') == 2
    for pin in ("gcc-13", "Xcode_16.4.app", "-arch=x64 -host_arch=x64"):
        assert workflow.count(pin) >= 2
    assert workflow.count("produce --hosted --fleet") == 2
    consumer = workflow.split("  consumer:\n", 1)[1].split("  profiles:\n", 1)[0]
    assert "actions/download-artifact@" in consumer and "actions/checkout@" not in consumer
    assert consumer.count("consume --hosted --fleet") == 3
    for contract in ("fleet-timing-smoke:", "fleet-evidence --hosted", "runtime-win-asan",
                     "runtime-darwin-tsan", "runtime-tsan", "verify.py features clang"):
        assert contract in workflow
    transfer(work, revision, fleet=True)


def fleet_receipts(work, revision):
    def rejected(action, text):
        try:
            action()
        except (KeyError, ValueError) as error:
            assert text in str(error), error
        else:
            raise AssertionError(f"Accepted invalid Fleet evidence: {text}")

    paced = {"status": "pass", "revision": revision, "stage": "native-paced", "hosted": False,
             "backend": "native-device-neutral", "instrumentation": "release"}
    driver.check_fleet_lane(paced, revision, "native-paced")
    rejected(lambda: driver.check_fleet_lane(paced, "f" * 40, "native-paced"), "revision")
    rejected(lambda: driver.check_fleet_lane({**paced, "instrumentation": "thread"}, revision, "native-paced"),
             "optimized")
    rejected(lambda: driver.check_fleet_lane({**paced, "hosted": True}, revision, "native-paced"), "native")
    diagnostic = {**paced, "stage": "diagnostic", "feature_available": True, "safe_control": True, "violation_control": True}
    driver.check_fleet_lane(diagnostic, revision, "diagnostic")
    for field in ("feature_available", "safe_control", "violation_control"):
        rejected(lambda: driver.check_fleet_lane({**diagnostic, field: False}, revision, "diagnostic"), "both controls")
    virtual = {**paced, "stage": "physical", "backend": "virtual", "selected_endpoint": "null"}
    rejected(lambda: driver.check_fleet_lane(virtual, revision, "physical"), "selected native endpoint")

    row = {
        "profile": "Interactive", "workload": "scheduled", "rate": 48000, "block": 128, "frames": 28800000,
        "elapsed_ns": 600000000000, "cold_first_included": True, "callbacks": 225000, "tiles": 450000,
        "max_consumer_ns": 1000, "max_active_ns_per_750_tiles": 400000000, "max_sealed_lead_frames": 384,
        "deadline_misses": 0, "transport_breaks": 0, "fallback_frames": 0, "unexpected_faults": 0,
        "late_events": 0, "unsubmitted_events": 0, "bad_stem_windows": 0, "queue_rejections": 0,
        "nonzero_windows": [599, 599], "overlap_completions": 10, "ticks0": 6000000, "ticks1": 18000000,
        "voice_snapshot_count": 1200, "stem0": 11, "stem1": 17, "criteria": {"independent_voice_pcm": True},
    }
    distribution = {"min": 50, "p50": 100, "p95": 200, "p99": 400, "p99.9": 800, "max": 1000}
    row["distribution_ns"] = {field: {**distribution, "count": row["tiles"] if field == "producer" else row["callbacks"]}
                              for field in driver.FLEET_TIMING_FIELDS}
    driver.check_fleet_timed_row(row)
    rejected(lambda: driver.check_fleet_timed_row({**row, "nonzero_windows": [599, 0]}), "productive")
    rejected(lambda: driver.check_fleet_timed_row({**row, "deadline_misses": 1}), "deadline")
    rejected(lambda: driver.check_fleet_timed_row({**row, "max_active_ns_per_750_tiles": 750000001}), "reserve")
    rejected(lambda: driver.check_fleet_timed_row({**row, "elapsed_ns": 2000000000}), "Incomplete")
    tails = deepcopy(row)
    del tails["distribution_ns"]["producer"]["p99.9"]
    rejected(lambda: driver.check_fleet_timed_row(tails), "p99.9")
    forged = work / "forged-study.json"
    forged.write_text(json.dumps({"protocol": "r3-v1", "status": "passed", "tree": "0" * 40, "input_sha256": {},
                                  "tuple": {"host": "native-device-neutral"}, "paced_runs": []}), encoding="utf-8")
    rejected(lambda: driver.inspect_fleet_study(forged, "1" * 40, {}), "tree")
    rejected(lambda: driver.inspect_fleet_study(forged, "0" * 40, {}), "Incomplete Fleet matrix")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = {"matrix": matrix, "transfer": transfer, "fleet-matrix": fleet_matrix, "fleet-receipts": fleet_receipts}
    parser.add_argument("mode", choices=modes)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=SOURCE, text=True).strip()
    evidence = os.environ.get("ENGINE_SIM_GATE_OUTPUT")
    if evidence:
        work = Path(evidence) / ("native-" + args.mode)
        work.mkdir(parents=True)
        modes[args.mode](work, revision)
    else:
        args.out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=args.out, prefix=args.mode + "-") as temporary:
            modes[args.mode](Path(temporary), revision)


if __name__ == "__main__":
    main()
