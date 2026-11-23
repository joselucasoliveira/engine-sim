"""Exercise native profile commands and independently consume real SDK transfers."""
import argparse
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


def consume_command(payload, root, revision, variant):
    args = ["consume", "--variant", variant, "--revision", revision, "--allow-dirty"]
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


def transfer(work, revision):
    for variant in ("static", "shared"):
        producer = work / (variant + "-producer")
        run([sys.executable, "-B", SOURCE / "tools/runtime_ci.py", "produce", "--variant", variant, "--revision", revision,
             "--out", producer, "--offline", "--allow-dirty"], SOURCE, work / (variant + "-producer.log"))
        payload = work / (variant + "-transfer")
        shutil.move(producer / "transfer", payload)
        manifest_path = payload / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        assert not any(name.startswith("share/engine-sim-runtime/example/") for name in manifest["sdk_files"])
        assert "fixture/main.json" in manifest["consumer_files"]
        root = work / (variant + "-consumer")
        root.mkdir()
        run(consume_command(payload, root, revision, variant), root, root / "consume.log")
        summary = json.loads((root / "run/result.json").read_text(encoding="utf-8"))
        assert summary["producer_trees_absent"] == (platform.system() == "Linux")
        assert summary["manifest_sha256"] == driver.sha256(manifest_path)
        rows = [row for row in summary["commands"] if "native_matrix" in row]
        assert len(rows) == 1 and len(rows[0]["native_matrix"]) == 2
        assert any("foreign_matrix" in row for row in summary["commands"]) == (variant == "shared")
        if variant == "static":
            # The same standalone transport entry point must stop before
            # compiling a client when a transferred artifact changes.
            damaged = work / "damaged-transfer"
            shutil.copytree(payload, damaged)
            with (damaged / "consumers.tar").open("ab") as stream:
                stream.write(b"tampered")
            failure = work / "tampered-consumer"
            failure.mkdir()
            run(consume_command(damaged, failure, revision, variant), failure, failure / "consume.log", expected=1)
            rejected = json.loads((failure / "run/result.json").read_text(encoding="utf-8"))
            assert rejected["error"] == "Transfer file identity differs: consumers.tar" and not (failure / "run/work").exists()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("matrix", "transfer"))
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=SOURCE, text=True).strip()
    evidence = os.environ.get("ENGINE_SIM_GATE_OUTPUT")
    if evidence:
        work = Path(evidence) / ("native-" + args.mode)
        work.mkdir(parents=True)
        (matrix if args.mode == "matrix" else transfer)(work, revision)
    else:
        args.out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=args.out, prefix=args.mode + "-") as temporary:
            (matrix if args.mode == "matrix" else transfer)(Path(temporary), revision)


if __name__ == "__main__":
    main()
