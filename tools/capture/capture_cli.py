"""Run isolated simulation cases and freeze reproducible signal comparisons."""

import argparse
from dataclasses import dataclass
import json
import math
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import time

from capture_metrics import digest, phase_lag, summarize_signal, summarize_trace, write_wave

QUICK = ("FIR", "GAS", "TONE")
REFERENCE = QUICK + ("KOHLER", "LS", "EJ25", "LOAD")
SUITES = {"quick": QUICK, "reference": REFERENCE, "stress": ("QUEUE", "LIFECYCLE", "THREAD")}
TRACE_COUNTS = {"GAS": 101, "TONE": 100, "TONE-QUIET": 100, "KOHLER": 1000, "LS": 1200, "EJ25": 1600}
PCM_CASES = {"TONE", "TONE-QUIET", "KOHLER", "LS", "EJ25"}
NUM_CALIBRATION = 20
NUM_HOLDOUT = 10


def invalid_number(value):
    raise ValueError(f"nonfinite value {value} in JSON")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"), parse_constant=invalid_number)


def save_json(path, record):
    path.write_text(json.dumps(record, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")


def absolute(value):
    path = Path(value)
    if not path.is_absolute():
        raise argparse.ArgumentTypeError(f"expected an absolute path: {path}")
    return path.resolve()


def fresh_output(path, source):
    if path.is_relative_to(source):
        raise ValueError(f"output must be outside the source checkout: {path}")
    path.mkdir(parents=True, exist_ok=True)
    if next(path.iterdir(), None) is not None:
        raise ValueError(f"output directory is not empty: {path}")


def parse_uint(value):
    parsed = int(value)
    if not 0 <= parsed <= 2**32 - 1:
        raise argparse.ArgumentTypeError("seed must be an unsigned 32-bit integer")
    return parsed


def parse_count(value):
    count = int(value)
    if count <= 0:
        raise argparse.ArgumentTypeError("repetitions must be positive")
    return count


def arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    actions = parser.add_subparsers(dest="action", required=True)
    run = actions.add_parser("run", allow_abbrev=False)
    run.add_argument("--suite", choices=SUITES, required=True)
    run.add_argument("--resources", required=True, type=absolute)
    run.add_argument("--out", required=True, type=absolute)
    run.add_argument("--seed", required=True, type=parse_uint)
    run.add_argument("--repetitions", required=True, type=parse_count)
    run.add_argument("--mode", choices=("sync", "threaded"), required=True)
    run.add_argument("--case")
    calibration = actions.add_parser("calibrate", allow_abbrev=False)
    calibration.add_argument("--baseline", type=absolute, required=True)
    calibration.add_argument("--out", type=absolute, required=True)
    compare = actions.add_parser("compare", allow_abbrev=False)
    compare.add_argument("--reference", type=absolute, required=True)
    compare.add_argument("--candidate", type=absolute, required=True)
    compare.add_argument("--policy", type=absolute, required=True)
    compare.add_argument("--out", type=absolute, required=True)
    return parser.parse_args(argv)


@dataclass
class WorkerInput:
    name: str
    resources: Path
    entry: Path = None
    diagnostic: str = None


def prepare_load_cases(resources, fixtures):
    fixtures.mkdir()
    unknown = fixtures / "unknown-node.mr"
    unknown.write_text('import "engine_sim.mr"\nnonexistent_test_node()\n', encoding="utf-8")
    missing_import = fixtures / "missing-import.mr"
    missing_import.write_text('import "nonexistent_module_test.mr"\n', encoding="utf-8")
    missing_ir = fixtures / "missing-ir"
    (missing_ir / "assets").mkdir(parents=True)
    shutil.copytree(resources / "es", missing_ir / "es")
    shutil.copytree(resources / "assets", missing_ir / "assets", dirs_exist_ok=True)
    (missing_ir / "es/sound-library/new/minimal_muffling_02.wav").unlink()
    return {
        "valid": WorkerInput("LOAD", resources),
        "missing-entry": WorkerInput("LOAD", resources, fixtures / "missing-entry.mr", "Can't find file"),
        "unknown-node": WorkerInput("LOAD", resources, unknown, "error R0010"),
        "missing-import": WorkerInput("LOAD", resources, missing_import, "missing-import(1): error"),
        "missing-ir": WorkerInput("LOAD", missing_ir, diagnostic="minimal_muffling_02.wav"),
    }


def source_metadata(binary, metadata):
    revision, tree, patch, compiler, os_tuple, configuration = metadata.read_text().splitlines()
    system = {
        "compiler": compiler, "target": os_tuple, "configuration": configuration,
        "platform": platform.platform(), "machine": platform.machine(), "libc": platform.libc_ver(),
        "python": platform.python_version(),
    }
    return {
        "revision": revision, "tree": tree, "tracked_delta_sha256": patch,
        "worker_sha256": digest(binary), "cli_sha256": digest(Path(__file__)),
        "metrics_sha256": digest(Path(__file__).with_name("capture_metrics.py")),
        "tuple": system,
    }


def input_hashes(output, root, expected_failure):
    record = output / "inputs.txt"
    if not record.exists():
        return {}
    inputs = {}
    for line in record.read_text(encoding="utf-8").splitlines():
        path = Path(line).resolve()
        if path.is_relative_to(output):
            key = "fixture/" + path.relative_to(output).as_posix()
        elif path.is_relative_to(root):
            key = "resources/" + path.relative_to(root).as_posix()
        else:
            raise ValueError(f"resolved input is outside the explicit resources or fixture root: {path}")
        if expected_failure and not path.is_file():
            inputs[key] = None
        else:
            inputs[key] = digest(path)
    return inputs


def artifact_hashes(root):
    return {file.name: digest(file) for file in sorted(root.iterdir()) if file.is_file()}


def execute_worker(binary, request, directory, seed, spectra):
    directory.mkdir()
    command = [str(binary), "--worker", request.name, str(request.resources), str(directory), str(seed)]
    if request.entry is not None:
        command.append(str(request.entry))
    output, error = directory / "stdout.log", directory / "stderr.log"
    event = {"argv": command, "cwd": str(directory)}
    start = time.monotonic()
    with output.open("wb") as stdout, error.open("wb") as stderr:
        try:
            result = subprocess.run(command, cwd=directory, stdout=stdout, stderr=stderr, timeout=90)
            event["exit"] = result.returncode
        except subprocess.TimeoutExpired:
            event["exit"] = 124
    event["process_seconds"] = time.monotonic() - start
    try:
        diagnostic = error.read_text(encoding="utf-8", errors="replace")
        if request.diagnostic is not None:
            if event["exit"] != 3 or request.diagnostic not in diagnostic or (directory / "graph.tsv").exists():
                raise ValueError(f"expected a rejected load with diagnostic {request.diagnostic!r}")
            event["expected_diagnostic"] = request.diagnostic
        elif event["exit"] != 0:
            raise ValueError(f"child exited {event['exit']}: {diagnostic.strip()}")
        else:
            validate_artifacts(directory, request.name, spectra, event)
        event["fixtures"] = input_hashes(directory, request.resources, request.diagnostic is not None)
        if request.entry is not None and request.entry.is_file():
            event["fixtures"]["fixture/input.mr"] = digest(request.entry)
        event["status"] = "pass"
    except (ValueError, OSError) as failure:
        event.update(status="fail", reason=str(failure))
    event["artifacts"] = artifact_hashes(directory)
    return event


def validate_artifacts(directory, name, spectra, event):
    raw = None
    if name == "FIR":
        raw = directory / "audio.f32le"
    elif name in PCM_CASES:
        raw = directory / "audio.s16le"
        if int((directory / "quantizer.txt").read_text()) != 0:
            raise ValueError("nonfinite sample encountered before quantization")
    if raw is not None:
        event["signal"] = summarize_signal(raw, digest(raw), spectra)
        if name == "FIR" and event["signal"]["frames"] != 44100:
            raise ValueError("incomplete FIR capture")
        if name.startswith("TONE") and event["signal"]["rail_count"]:
            raise ValueError("tone rail hit")
        write_wave(raw, directory / "audio.wav")
    process = directory / "process.tsv"
    if process.exists():
        user, system, rss = (float(value) for value in process.read_text().split())
        if not all(math.isfinite(value) and value >= 0 for value in (user, system, rss)):
            raise ValueError("invalid resource usage")
        event["process"] = {"user_seconds": user, "system_seconds": system, "max_rss_bytes": rss}
    startup = directory / "startup.tsv"
    if startup.exists():
        parse_us, init_us = (float(value) for value in startup.read_text().split())
        if not all(math.isfinite(value) and value >= 0 for value in (parse_us, init_us)):
            raise ValueError("invalid startup duration")
        event["startup"] = {"parse_us": parse_us, "initialize_us": init_us}
    if name in TRACE_COUNTS:
        event["trace"] = summarize_trace(directory / "trace.csv")
        if event["trace"]["rows"] != TRACE_COUNTS[name]:
            raise ValueError("incomplete trace capture")
    if name in {"KOHLER", "LS", "EJ25", "LOAD"}:
        graph = directory / "graph.tsv"
        if not graph.read_text().strip():
            raise ValueError("missing engine graph")
        event["graph"] = digest(graph)


def run_cases(options, binary, metadata, source):
    resources = options.resources
    if not all((resources / name).is_dir() for name in ("assets", "es")):
        raise ValueError("resources must contain assets and es directories")
    pool = SUITES[options.suite]
    if options.case is not None and options.case not in pool:
        raise ValueError(f"case {options.case!r} is outside suite {options.suite!r}")
    cases = (options.case,) if options.case else pool
    fresh_output(options.out, source)
    fixtures = prepare_load_cases(resources, options.out / "load-fixtures") if "LOAD" in cases else None
    run = {
        "version": 1, "selection": list(cases), "suite": options.suite, "mode": options.mode,
        "resources": str(resources), "seed": options.seed, "repetitions": options.repetitions,
        "build": source_metadata(binary, metadata), "cases": {},
        "time_class": "diagnostic on shared hardware",
    }
    spectra = {}
    for case in cases:
        transactions = []
        run["cases"][case] = transactions
        if case in SUITES["stress"] or (case in PCM_CASES and options.mode == "threaded"):
            transactions.append({"status": "fail", "reason": f"{case} requires a qualified lifetime, buffer or worker path"})
            continue
        attempts = {"main": WorkerInput(case, resources)}
        if case == "TONE":
            attempts["quiet"] = WorkerInput("TONE-QUIET", resources)
        elif case == "LOAD":
            attempts = fixtures
        for number in range(1, options.repetitions + 1):
            trial = {}
            transactions.append(trial)
            for variant, request in attempts.items():
                directory = options.out / case / f"{number:03d}" / variant
                directory.parent.mkdir(parents=True, exist_ok=True)
                event = execute_worker(binary, request, directory, options.seed, spectra)
                event["directory"] = directory.relative_to(options.out).as_posix()
                trial[variant] = event
            # Leave complete receipts available even when an attempt fails.
            save_json(options.out / "run.json", run)
    failures = []
    for case, trials in run["cases"].items():
        for trial in trials:
            events = [trial] if "status" in trial else trial.values()
            failures.extend({"case": case, "reason": event["reason"]} for event in events if event["status"] != "pass")
    run["failures"] = failures
    run["status"] = "fail" if failures else "pass"
    save_json(options.out / "run.json", run)
    if failures:
        raise ValueError(f"{failures[0]['case']}: {failures[0]['reason']}")


def load_run(root):
    run = read_json(root / "run.json")
    selection = run.get("selection")
    if run.get("version") != 1 or run.get("status") != "pass":
        raise ValueError("run failed or was incomplete")
    if not selection or set(selection) != run["cases"].keys() or len(set(selection)) != len(selection):
        raise ValueError("case selection is empty or differs from the recorded trials")
    for trials in run["cases"].values():
        if len(trials) != run["repetitions"] or any(not trial for trial in trials):
            raise ValueError("incomplete repetitions or variant selection")
        for trial in trials:
            for event in trial.values():
                directory = root / event["directory"]
                if not directory.resolve().is_relative_to(root):
                    raise ValueError("recorded run artifact escapes its root")
                artifacts = artifact_hashes(directory)
                if event["status"] != "pass" or event["artifacts"] != artifacts:
                    raise ValueError(f"incomplete or modified artifacts in {directory}")
                if "signal" in event:
                    raw_name = "audio.f32le" if "audio.f32le" in artifacts else "audio.s16le"
                    sample_bytes = 4 if raw_name.endswith(".f32le") else 2
                    if event["signal"]["sha256"] != artifacts.get(raw_name) or (
                        event["signal"]["frames"] * sample_bytes != (directory / raw_name).stat().st_size
                    ):
                        raise ValueError(f"inconsistent waveform evidence in {directory}")
                if "trace" in event:
                    actual_trace = summarize_trace(directory / "trace.csv")
                    if event["trace"] != actual_trace:
                        raise ValueError(f"inconsistent trace evidence in {directory}")
    return run


def observation(trial):
    return {
        variant: {
            "fixtures": event["fixtures"],
            "diagnostic": event.get("expected_diagnostic"),
            "exit": event["exit"],
            "graph": event.get("graph"),
            "signal": None if "signal" not in event else {
                name: event["signal"][name] for name in ("frames", "sample_rate", "sha256")
            },
            "trace": None if "trace" not in event else {
                name: event["trace"][name] for name in ("rows", "physical_sha256")
            },
        } for variant, event in trial.items()
    }


def fit_calibration(run):
    minimum = NUM_CALIBRATION + NUM_HOLDOUT
    if run["repetitions"] < minimum or run["mode"] != "sync":
        raise ValueError(f"calibration requires at least {minimum} complete sync repetitions")
    cases = {}
    holdouts = {}
    for case, trials in run["cases"].items():
        original = observation(trials[0])
        if any(observation(trial) != original for trial in trials[:NUM_CALIBRATION]):
            raise ValueError(f"{case} calibration signals are not repeatable on the measured tuple")
        holdout = trials[NUM_CALIBRATION:]
        if any(observation(trial) != original for trial in holdout):
            raise ValueError(f"{case} holdout signals differ from the frozen repeatable calibration set")
        cases[case] = original
        holdouts[case] = len(holdout)
    return cases, holdouts


def calibrate(options, source):
    baseline = load_run(options.baseline)
    fresh_output(options.out, source)
    cases, holdouts = fit_calibration(baseline)
    policy = {
        "version": 1, "baseline_sha256": digest(options.baseline / "run.json"),
        "tuple": baseline["build"]["tuple"], "seed": baseline["seed"], "mode": baseline["mode"],
        "calibration_repetitions": NUM_CALIBRATION, "holdouts": holdouts,
        "comparison": "exact same-seed PCM and aligned physical trace on this measured tuple",
        "intentional_allowances": [], "cases": cases,
        "timing": "measured separately, no dedicated-hardware timing envelope admitted",
    }
    path = options.out / "policy.json"
    save_json(path, policy)
    save_json(options.out / "holdout.json", {"status": "pass", "cases": holdouts, "policy_sha256": digest(path)})


def compare_trials(policy, candidate):
    mismatches = []
    for name, expected in (("seed", policy["seed"]), ("mode", policy["mode"])):
        if candidate[name] != expected:
            mismatches.append({"item": name, "expected": expected, "actual": candidate[name]})
    if candidate["build"]["tuple"] != policy["tuple"]:
        mismatches.append({"item": "measurement tuple differs"})
    for case, trials in candidate["cases"].items():
        if case not in policy["cases"]:
            mismatches.append({"case": case, "reason": "no frozen reference"})
            continue
        expected = policy["cases"][case]
        for index, trial in enumerate(trials):
            actual = observation(trial)
            if actual != expected:
                mismatches.append({"case": case, "trial": index + 1, "expected": expected, "actual": actual})
    return mismatches


def compare(options, source):
    reference = load_run(options.reference)
    candidate = load_run(options.candidate)
    policy = read_json(options.policy)
    if policy["version"] != 1 or policy["baseline_sha256"] != digest(options.reference / "run.json"):
        raise ValueError("policy does not identify this reference")
    if set(candidate["selection"]) != set(reference["selection"]):
        raise ValueError("candidate and reference case selections differ")
    fresh_output(options.out, source)
    mismatches = compare_trials(policy, candidate)
    lags = {}
    for case, trials in candidate["cases"].items():
        lags[case] = {}
        for variant, event in trials[0].items():
            if not event.get("signal"):
                continue
            before = reference["cases"][case][0][variant]
            if "signal" not in before:
                continue
            if before["signal"]["sha256"] == event["signal"]["sha256"]:
                lags[case][variant] = 0 if event["signal"]["peak"] > 0 else None
            else:
                suffix = "f32le" if case == "FIR" else "s16le"
                lags[case][variant] = phase_lag(
                    options.reference / before["directory"] / f"audio.{suffix}",
                    options.candidate / event["directory"] / f"audio.{suffix}")
    save_json(options.out / "comparison.json", {
        "status": "fail" if mismatches else "pass", "mismatches": mismatches,
        "phase_lag_frames": lags, "policy_sha256": digest(options.policy),
        "reference_sha256": digest(options.reference / "run.json"),
        "candidate_sha256": digest(options.candidate / "run.json"),
        "intentional_allowances": policy["intentional_allowances"],
    })
    if mismatches:
        raise ValueError("candidate differs from the frozen comparison")


def self_test():
    with tempfile.TemporaryDirectory(prefix="engine-capture-self-test-") as name:
        root = Path(name)
        pcm = root / "audio.s16le"
        pcm.write_bytes(struct.pack("<2048h", *[0] * 2048))
        silent = summarize_signal(pcm, digest(pcm), {})
        assert silent["rms"] == 0 and silent["leading_zero_frames"] == 2048
        assert all(value == 0 for value in silent["spectrum"]["mean_band_power"])
        candidate_path = root / "candidate.s16le"
        candidate_path.write_bytes(pcm.read_bytes()[:2] + struct.pack("<h", 1) + pcm.read_bytes()[4:])
        changed = summarize_signal(candidate_path, digest(candidate_path), {})
        event = {"fixtures": {"pulse": "input-digest"}, "exit": 0, "signal": silent}
        reference = {"seed": 1, "mode": "sync", "repetitions": 30, "build": {"tuple": {}},
                     "cases": {"FIR": [{"main": event} for _ in range(30)]}}
        policy = {"seed": 1, "mode": "sync", "tuple": {}, "cases": fit_calibration(reference)[0]}
        assert not compare_trials(policy, reference)
        for altered in (
            {**event, "signal": changed},
            {**event, "signal": {**silent, "frames": 2047}},
            {**event, "fixtures": {"pulse": "wrong-input-digest"}},
        ):
            assert compare_trials(policy, {**reference, "cases": {"FIR": [{"main": altered}]}})
        assert compare_trials(policy, {**reference, "seed": 2})
        nan = root / "invalid.f32le"
        nan.write_bytes(struct.pack("<f", math.nan))
        try:
            summarize_signal(nan, digest(nan), {})
        except ValueError:
            pass
        else:
            raise AssertionError("nonfinite signal was accepted")


def main():
    binary, metadata, source = (Path(value).resolve() for value in sys.argv[1:4])
    argv = sys.argv[4:]
    if argv == ["--self-test"]:
        self_test()
    elif argv == ["--quick-test"]:
        with tempfile.TemporaryDirectory(prefix="engine-capture-quick-", dir=metadata.parent) as scratch:
            options = arguments(["run", "--suite", "quick", "--resources", str(source),
                                 "--out", scratch, "--seed", "1", "--repetitions", "1", "--mode", "sync"])
            run_cases(options, binary, metadata, source)
    else:
        options = arguments(argv)
        if options.action == "run":
            run_cases(options, binary, metadata, source)
        elif options.action == "calibrate":
            calibrate(options, source)
        else:
            compare(options, source)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(json.dumps({"status": "fail", "reason": str(error)}), file=sys.stderr)
        sys.exit(1)
