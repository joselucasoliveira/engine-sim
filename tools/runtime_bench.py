"""Measure optimized runtime costs; keep diagnostic probes and old references separate."""
import argparse
import concurrent.futures
import csv
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile
import time


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def absolute(value):
    path = Path(value)
    if not path.is_absolute():
        raise argparse.ArgumentTypeError("Use an absolute path")
    return path.resolve()


def measurements(path):
    result = {}
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.reader(stream):
            if len(row) != 2 or row[0] in result:
                raise ValueError("Invalid or duplicate measurement row")
            key, text = row
            if key == "pipeline":
                result[key] = text
            else:
                value = float(text) if key.endswith("seconds") or key.endswith("crank_rad_s") else int(text)
                if not math.isfinite(value):
                    raise ValueError("Nonfinite benchmark measurement")
                result[key] = value
    if result.get("version") != 1:
        raise ValueError("Unsupported benchmark schema")
    return result


def conditions():
    record = {"platform": platform.platform(), "machine": platform.machine(), "python": platform.python_version(),
              "affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None}
    for name, path in (("cpuinfo", "/proc/cpuinfo"), ("governor", "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
                       ("current_khz", "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq")):
        try:
            text = Path(path).read_text()
            record[name] = text if name != "cpuinfo" else "\n".join(line for line in text.splitlines() if any(key in line for key in ("model name", "cpu MHz", "microcode")))
        except OSError:
            record[name] = None
    return record


def run(executable, model, folder, rate, block, instances, pipeline, schedule, cores):
    folder.mkdir()
    jobs = instances if pipeline == "old" else 1
    processes, commands, outputs, errors = [], [], [], []
    try:
        for index in range(jobs):
            output = folder / f"measure-{index}.csv"
            argv = [str(executable), str(model["entry"]), str(model["library"]), str(model["assets"]), str(output),
                    str(rate), str(block), str(1 if pipeline == "old" else instances), pipeline, "200", "25", "1", schedule]
            affinity = [cores[index % len(cores)]] if pipeline == "old" and cores else cores[:instances]
            preexec = (lambda chosen=affinity: os.sched_setaffinity(0, chosen)) if affinity and hasattr(os, "sched_setaffinity") else None
            stderr = (folder / f"stderr-{index}.log").open("wb")
            errors.append(stderr)
            process = subprocess.Popen(argv, cwd=folder, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
                                       preexec_fn=preexec, env={**os.environ, "LC_ALL": "C", "TZ": "UTC"})
            processes.append(process); outputs.append(output)
            commands.append({"argv": argv, "cwd": str(folder), "affinity": affinity})
        # Start processes before creating reader threads: no Python preexec hook
        # is called while this function owns a live thread pool.
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            waiting = [pool.submit(process.stdout.readline) for process in processes]
            try:
                for future in waiting:
                    if future.result(timeout=120) != b"READY\n":
                        raise ValueError("Benchmark failed before the READY barrier")
            except BaseException:
                for process in processes:
                    process.kill()
                raise
        started = time.monotonic()
        for process in processes:
            process.stdin.write(b"GO\n"); process.stdin.flush()
        for index, process in enumerate(processes):
            stdout, _ = process.communicate(timeout=180)
            (folder / f"stdout-{index}.log").write_bytes(b"READY\n" + stdout)
            commands[index]["exit"] = process.returncode
            if process.returncode:
                raise ValueError(f"Benchmark process exited {process.returncode}")
        wall = time.monotonic() - started
    except BaseException:
        for process in processes:
            if process.poll() is None:
                process.kill()
            process.wait()
        raise
    finally:
        for error in errors:
            error.close()
        save(folder / "commands.json", commands)
    records = [measurements(path) for path in outputs]
    pcm, render_times = [], []
    for index, (path, record) in enumerate(zip(outputs, records)):
        if pipeline in ("cycles", "convert"):
            continue
        if record.get("instances") != (1 if pipeline == "old" else instances) or record.get("rate") != rate:
            raise ValueError("Measurement workload differs from requested parameters")
        for instance in range(record["instances"]):
            raw = Path(str(path) + f".{instance}.f32le")
            if raw.stat().st_size != rate // 100 * 25 * 4 or record[f"{instance}.nonzero_frames"] <= 0:
                raise ValueError("Incomplete or silent measurement output")
            pcm.append({"path": str(raw), "sha256": digest(raw)})
            render_times.append(record[f"{instance}.render_seconds"])
    result = {"pipeline": pipeline, "processes": records, "pcm": pcm, "commands": commands,
              "driver_wall_seconds": wall, "cpu_seconds": sum(record["process_cpu_seconds"] for record in records),
              "sum_process_peak_rss_bytes": sum(record["peak_rss_bytes"] for record in records)}
    if render_times:
        result["max_owner_render_seconds"] = max(render_times)
        result["simulated_seconds_per_wall_second"] = instances * .25 / max(render_times)
    save(folder / "result.json", result)
    return result


def distribution(values):
    values = sorted(values)
    q1, _, q3 = statistics.quantiles(values, n=4, method="inclusive") if len(values) > 1 else (values[0], values[0], values[0])
    return {"n": len(values), "minimum": values[0], "q1": q1, "median": statistics.median(values), "q3": q3,
            "maximum": values[-1], "sample_sd": statistics.stdev(values) if len(values) > 1 else None}


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--benchmark", type=absolute, required=True)
    parser.add_argument("--probe", type=absolute, required=True)
    parser.add_argument("--out", type=absolute, required=True)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--fixture", type=absolute)
    source.add_argument("--resources", type=absolute)
    parser.add_argument("--reference", type=absolute)
    parser.add_argument("--reference-provenance", type=absolute)
    parser.add_argument("--compile-database", type=absolute)
    parser.add_argument("--repetitions", type=int, default=10)
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    resources = args.fixture or args.resources
    if args.out.is_relative_to(root) or args.out.is_relative_to(resources):
        parser.error("Benchmark output must be outside source and resources")
    if args.smoke:
        args.out.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="runtime-bench-", dir=args.out))
        repetitions = 1
    else:
        if args.repetitions < 10:
            parser.error("The cost study requires at least ten paired repetitions")
        args.out.mkdir(parents=True, exist_ok=True)
        if any(args.out.iterdir()):
            parser.error("Benchmark output must be fresh and empty")
        output, repetitions = args.out, args.repetitions
    if bool(args.reference) != bool(args.reference_provenance):
        parser.error("An independent reference requires its supplied provenance")
    if args.reference:
        provenance = json.loads(args.reference_provenance.read_text())
        if provenance.get("reference_executable_sha256") != digest(args.reference) or not provenance.get("self_check_passed"):
            parser.error("Independent reference identity/self-check differs")
    inputs = output / "inputs"; inputs.mkdir()
    if args.fixture:
        models = [{"name": "fixture", "entry": args.fixture / "main.mr", "library": args.fixture / "library", "assets": args.fixture}]
        schedule = "fixture"
    else:
        models, schedule = [], "legacy"
        for name, module in (("KOHLER", "engines/atg-video-1/02_kohler_ch750.mr"), ("LS", "engines/atg-video-2/07_gm_ls.mr")):
            entry = inputs / (name + ".mr")
            entry.write_text(f'import "engine_sim.mr"\nimport "{module}"\nmain()\n', encoding="utf-8")
            models.append({"name": name, "entry": entry, "library": args.resources / "es", "assets": args.resources / "assets"})
    environment = conditions()
    identities = sorted({path.resolve() for folder in (resources / "es", resources / "assets") if folder.is_dir()
                         for path in folder.rglob("*") if path.is_file() and path.suffix.lower() in (".mr", ".wav")})
    if args.fixture:
        identities = sorted(path for path in args.fixture.rglob("*") if path.is_file())
    save(output / "input-identities.json", {str(path): digest(path) for path in identities})
    cores = (environment["affinity"] or [])[:2]
    report = {"version": 1, "status": "running", "smoke": args.smoke, "conditions": environment,
              "binaries": {str(path): digest(path) for path in (args.benchmark, args.probe, *([args.reference] if args.reference else []))},
              "reference_provenance": str(args.reference_provenance) if args.reference else None,
              "reference_provenance_sha256": digest(args.reference_provenance) if args.reference else None,
              "input_identities_sha256": digest(output / "input-identities.json"),
              "cases": [], "profiles": [], "limits": [
                  "Optimized and instrumented probe results are separate; no callback deadline or universal performance threshold.",
                  "Cold means first preparation in a new process; filesystem inputs are warmed/read for provenance.",
                  "Two old independent engines require two processes because of global RNG; new engines use one process and two handles/threads.",
                  "Old 48kHz output is an explicitly added consumer converter over old native 44100 PCM.",
                  "Peak RSS sums for multiple old processes are sums of individual high-water values, not a simultaneous RSS sample.",
                  "Mutex wrapping covers owned/static pthread lock call sites; dynamic system-library locks and C allocator internals are not counted."]}
    report["limits"] += ["Allocation probes count ordinary C++ new/new[]; aligned allocation and direct C allocation are outside this counter.",
                         "Current RSS is available on Linux and Windows; macOS currently reports zero for that unavailable field, with peak RSS still available.",
                         "Existing solver timers report a rolling 600-step average with integer microsecond resolution; they are not whole-interval stage totals.",
                         "Old and new render timings include changed RNG, scheduling and solver topology; their ratio alone cannot isolate a clock or RNG implementation cost."]
    if args.compile_database and args.compile_database.is_file():
        report["compile_database_sha256"] = digest(args.compile_database)
        report["benchmark_compile_commands"] = [row for row in json.loads(args.compile_database.read_text()) if row["file"].endswith("/benchmark.cpp")]
    save(output / "run.json", report)
    configs = [(44100, 1, 1), (48000, 256, 2), (44100, 1024, 1)] if args.smoke else list(itertools.product((44100, 48000), (1, 256, 1024), (1, 2)))
    try:
        for model in models:
            for rate, block, instances in configs:
                case = {"model": model["name"], "rate": rate, "block": block, "instances": instances, "pairs": []}
                case_root = output / f'{model["name"]}-{rate}-{block}-{instances}'; case_root.mkdir()
                for repeat in range(repetitions):
                    pair = {"repetition": repeat, "conditions_before": conditions(), "runs": {}}
                    order = (["old"] if args.reference else []) + ["public", "core"]
                    if repeat % 2:
                        order.reverse()
                    for variant in order:
                        executable = args.reference if variant == "old" else args.benchmark
                        pair["runs"][variant] = run(executable, model, case_root / f"{repeat}-{variant}", rate, block, instances, variant, schedule, cores)
                    if [pcm["sha256"] for pcm in pair["runs"]["public"]["pcm"]] != [pcm["sha256"] for pcm in pair["runs"]["core"]["pcm"]]:
                        raise ValueError("Public and private production pipelines differ on the same timeline")
                    pair["conditions_after"] = conditions(); case["pairs"].append(pair)
                    save(case_root / "case.json", case)
                case["distributions"] = {variant: distribution([pair["runs"][variant]["max_owner_render_seconds"] for pair in case["pairs"]]) for variant in order}
                case["paired_ratios"] = {f"public_over_{variant}": distribution([
                    pair["runs"]["public"]["max_owner_render_seconds"] / pair["runs"][variant]["max_owner_render_seconds"]
                    for pair in case["pairs"]]) for variant in ("core", *(["old"] if args.reference else []))}
                report["cases"].append(case); save(output / "run.json", report)
            # One separately instrumented stage/allocation/mutex pass per rate.
            for rate in (44100, 48000):
                probe = run(args.probe, model, output / f'{model["name"]}-probe-{rate}', rate, 256, 1, "profile", schedule, cores)
                report["profiles"].append({"model": model["name"], "rate": rate, "result": probe})
            cycle = run(args.probe, model, output / f'{model["name"]}-cycles', 44100, 256, 1, "cycles", schedule, cores)
            if cycle["processes"][0]["cycles"] != 100:
                raise ValueError("Incomplete retained-memory observation")
            report.setdefault("lifecycle", []).append({"model": model["name"], "result": cycle})
            # Isolated conversion uses the preceding source value and the same
            # half-open measured native interval, with source time kept fixed.
            native_case = next(case for case in report["cases"] if case["model"] == model["name"] and case["rate"] == 44100 and case["instances"] == 1)
            source_pcm = Path(native_case["pairs"][0]["runs"]["public"]["pcm"][0]["path"])
            source_file = output / (model["name"] + "-native-with-previous.f32le")
            source_file.write_bytes(Path(str(source_pcm) + ".previous").read_bytes() + source_pcm.read_bytes())
            for rate in (44100, 48000):
                converted_model = {**model, "entry": source_file}
                conversion = run(args.benchmark, converted_model, output / f'{model["name"]}-convert-{rate}', rate, 256, 1, "convert", schedule, cores)
                target = Path(conversion["commands"][0]["argv"][4] + ".0.f32le")
                expected_case = next(case for case in report["cases"] if case["model"] == model["name"] and case["rate"] == rate)
                expected = expected_case["pairs"][0]["runs"]["public"]["pcm"][0]["sha256"]
                if digest(target) != expected:
                    raise ValueError("Isolated converter differs from the real public timeline")
                report.setdefault("conversion", []).append({"model": model["name"], "rate": rate, "source_sha256": digest(source_file), "result": conversion, "exact_pcm": True})
        if any(digest(path) != expected for path, expected in ((Path(name), value) for name, value in json.loads((output / "input-identities.json").read_text()).items())):
            raise ValueError("Benchmark resource identities changed during the study")
        report.update(status="pass", historical_comparison="paired independent reference supplied" if args.reference else "not run")
    except BaseException as error:
        report.update(status="fail", reason=str(error)); raise
    finally:
        save(output / "run.json", report)
    print(f"benchmark artifacts: {output}")
    print(f'{len(report["cases"])} configurations; {repetitions} paired repetitions; public/private PCM and isolated conversion exact')


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError, TimeoutError, concurrent.futures.TimeoutError) as error:
        print(str(error), file=sys.stderr); sys.exit(1)
