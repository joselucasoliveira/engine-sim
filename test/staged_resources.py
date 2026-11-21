"""Exercise the installed capture tool with a relocated, immutable resource tree."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile


def hashes(root):
    return {file.relative_to(root).as_posix(): hashlib.sha256(file.read_bytes()).hexdigest()
            for file in sorted(root.rglob("*")) if file.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--config", required=True)
    options = parser.parse_args()
    evidence = os.environ.get("ENGINE_SIM_STAGE_EVIDENCE")
    if evidence:
        Path(evidence).mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix="engine-sim-stage-", dir=evidence))
    print(f"staging evidence: {scratch}", flush=True)
    assert not scratch.is_relative_to(options.build.resolve())
    assert not scratch.is_relative_to(options.source.resolve())
    sequence = 0

    def run(command, *, expected=0, cwd=None, environment=None):
        nonlocal sequence
        sequence += 1
        result = subprocess.run([str(value) for value in command], cwd=cwd, env=environment,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=180)
        record = scratch / f"command-{sequence:02d}"
        record.with_suffix(".json").write_text(json.dumps({"argv": list(map(str, command)),
            "cwd": str(cwd), "exit": result.returncode, "expected_exit": expected}, indent=2))
        record.with_suffix(".stdout").write_bytes(result.stdout)
        record.with_suffix(".stderr").write_bytes(result.stderr)
        if (result.returncode == 0) != (expected == 0):
            raise AssertionError(f"command {sequence} exited {result.returncode}: "
                                 + result.stderr.decode("utf-8", errors="replace"))
        return result

    install = [options.cmake, "--install", options.build, "--config", options.config, "--prefix"]
    environment = dict(os.environ, ENGINE_SIM_STAGE_MODE="redistributable")
    denied = scratch / "denied"
    rejected = run(install + [denied], expected=1, environment=environment)
    assert b"unresolved impulse-recording clearance" in rejected.stderr
    assert not denied.exists() or not any(denied.rglob("*")), "rights preflight copied files"
    environment["ENGINE_SIM_STAGE_MODE"] = "local-validation"
    original, second = scratch / "original", scratch / "second"
    run(install + [original], environment=environment)
    run(install + [second], environment=environment)
    assert hashes(original) == hashes(second), "fresh installs differ"
    stage = scratch / "relocated space \u03a9 \u00e7"
    original.rename(stage)
    resources = stage / "share/engine-sim"
    assert (resources / "LOCAL_VALIDATION_ONLY.txt").is_file()
    inventory = json.loads((stage / "runtime-inventory.json").read_text())
    assert inventory["mode"] == "local-validation"
    before = hashes(stage)
    assert set(inventory["files"]) == before.keys() - {"runtime-inventory.json"}
    for name, record in inventory["files"].items():
        assert record["sha256"] == before[name]
        assert record["bytes"] == (stage / name).stat().st_size
    manifest = json.loads((options.source / "cmake/runtime-assets.json").read_text())
    for row in manifest["files"]:
        if row["component"] == "base":
            assert before["share/engine-sim/" + row["destination"]] == row["sha256"]
    for name in ("licenses/engine-sim-MIT.txt", "licenses/THIRD_PARTY_NOTICES.md", "licenses/solver-MIT.txt",
                 "licenses/Piranha-MIT.txt", "licenses/Boost.txt", "licenses/FlexLexer-notice.h"):
        assert (resources / name).is_file(), name
    assert not list(stage.rglob("*.dia"))
    assert len(list((resources / "es/sound-library").rglob("*.wav"))) == 10
    decoy = scratch / "unrelated cwd"
    (decoy / "assets").mkdir(parents=True)
    (decoy / "es").mkdir()
    (decoy / "assets/main.mr").write_text("invalid_decoy()\n")
    child_environment = dict(os.environ, PATH=str(stage / "bin") + os.pathsep + os.environ["PATH"])
    namespace = []
    if platform.system() == "Linux":
        command = json.loads(os.environ.get("ENGINE_SIM_BWRAP_COMMAND", '["bwrap"]'))
        assert isinstance(command, list) and command and all(isinstance(arg, str) for arg in command)
        if shutil.which(command[0]) is None:
            raise AssertionError("bubblewrap is required for the source-inaccessible Linux staging test")
        namespace = command + ["--unshare-net", "--ro-bind", "/", "/", "--bind", str(scratch), str(scratch),
            "--ro-bind", str(stage), str(stage), "--tmpfs", str(options.source.resolve()),
            "--tmpfs", str(options.build.resolve()), "--chdir", str(decoy), "--"]
        probe = """import errno, pathlib, sys
assert not list(pathlib.Path(sys.argv[1]).iterdir())
assert not list(pathlib.Path(sys.argv[2]).iterdir())
try:
    (pathlib.Path(sys.argv[3]) / 'write-probe').write_text('unexpected')
except OSError as error:
    assert error.errno in (errno.EROFS, errno.EACCES), error
else:
    raise AssertionError('installed tree is writable')
"""
        run(namespace + [sys.executable, "-B", "-c", probe, options.source, options.build, stage],
            cwd=decoy, environment=child_environment)
    else:
        print("Source-masking and immutable-mount qualification pending on this native platform", flush=True)
    binary = "engine-sim-capture.exe" if os.name == "nt" else "engine-sim-capture"
    if os.name == "nt":
        # Windows CreateProcess does not use the supplied child PATH for executable lookup.
        binary = shutil.which(binary, path=child_environment["PATH"])
        assert binary is not None
    command = namespace + [binary, "run", "--suite", "reference", "--case", "LOAD",
        "--seed", "1", "--repetitions", "1", "--mode", "sync", "--out"]
    result_root = scratch / "default-run"
    run(command + [result_root], cwd=decoy, environment=child_environment)
    result = json.loads((result_root / "run.json").read_text())
    assert result["status"] == "pass" and result["resources"] == str(resources)
    trial = result["cases"]["LOAD"][0]
    assert set(trial) == {"valid", "missing-entry", "unknown-node", "missing-import", "missing-ir"}
    for event in trial.values():
        assert event["status"] == "pass"
    assert trial["valid"]["fixtures"]
    valid = result_root / trial["valid"]["directory"]
    for line in (valid / "inputs.txt").read_text(encoding="utf-8").splitlines():
        assert Path(line).is_relative_to(resources), line
    assert (valid / "graph.tsv").stat().st_size > 0
    missing_ir = result_root / trial["missing-ir"]["directory"] / "stderr.log"
    diagnostic = missing_ir.read_text(encoding="utf-8")
    assert str(result_root / "load-fixtures/missing-ir/es/sound-library/new/minimal_muffling_02.wav") in diagnostic
    # Resolve explicit relative CLI paths before the helper changes cwd for each worker.
    relative_resources = os.path.relpath(resources, decoy)
    relative_script = os.path.relpath(resources / "assets/main.mr", decoy)
    run(command + [scratch / "explicit-run", "--resources", relative_resources, "--script", relative_script],
        cwd=decoy, environment=child_environment)
    missing = scratch / "absent resources \u03a9"
    failure = run(command + [scratch / "missing-run", "--resources", missing], expected=1,
                  cwd=decoy, environment=child_environment)
    assert str(missing) in json.loads(failure.stderr)["reason"]
    assert hashes(stage) == before, "installed tree was changed during execution"
    assert hashes(decoy) == {"assets/main.mr": hashlib.sha256(b"invalid_decoy()\n").hexdigest()}
    (scratch / "result.json").write_text(json.dumps({"status": "pass", "files": len(before),
        "source_inaccessible": bool(namespace), "stage_immutable_mount": bool(namespace),
        "source": str(options.source), "build": str(options.build), "stage": str(stage)}, indent=2))
    print("relocated installation and actual default engine graph passed", flush=True)


if __name__ == "__main__":
    main()
