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
import signal
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
            sources = model["old"] if pipeline == "old" else (model["entry"], model["assets"])
            argv = [str(executable), *map(str, sources), str(output),
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
              "measurement_files": list(map(str, outputs)),
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


def cost_environment(cores):
    files = {f"cpu{core}_khz": Path(f"/sys/devices/system/cpu/cpu{core}/cpufreq/scaling_cur_freq")
             for core in cores}
    files.update({str(path): path for path in Path("/sys/class/thermal").glob("thermal_zone*/temp")})
    files["load_average"] = Path("/proc/loadavg")
    return {name: path.read_text().strip() for name, path in files.items() if path.is_file()}


def installed_trial(out, archive, executable, inputs, case, env, baseline=None, transients=False, launcher=None):
    model, rate, mode, block, owners = case
    out.mkdir()
    target = out / "result.csv"
    schedule = "fixture" if model == "fixture" else "legacy"
    args = [str(executable), *map(str, inputs), str(target), str(rate), str(block), str(owners)]
    if baseline is None:
        cores, pin = [], False
    else:
        cores = baseline["affinity"][:owners]
        pin = baseline["conditions"].get("worker_placement") == "fixed_cpu_at_READY"
    if len(inputs) == 3:
        args.extend(("public", "200", "100", "403", schedule, mode))
    else:
        args.extend((schedule, mode, "transients" if transients else "steady"))
    if launcher:
        args.insert(0, str(launcher))
    if cores:
        args[:0] = ["taskset", "-c", ",".join(map(str, cores))]
    command = {"argv": args, "cwd": str(out), "affinity": cores}
    save(out / "command.json", command)
    with (out / "stderr.log").open("wb") as error, subprocess.Popen(
            args, cwd=out, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=error,
            start_new_session=bool(launcher)) as child:
        command["pid"] = child.pid

        def stop():
            if launcher:
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            elif child.poll() is None:
                child.kill()

        try:
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                try:
                    ready = pool.submit(child.stdout.readline).result(timeout=120)
                    if ready.rstrip(b"\r\n") != b"READY":
                        raise ValueError("Cost consumer did not reach READY")
                except BaseException:
                    stop()
                    raise
            pid = child.pid
            if launcher:
                children = Path(f"/proc/{pid}/task/{pid}/children").read_text().split()
                if len(children) != 1:
                    raise ValueError("Expected one benchmark process inside the RSS launcher")
                pid = int(children[0])
                command["benchmark_pid"] = pid
            if pin:
                tasks = Path(f"/proc/{pid}/task").iterdir()
                workers = sorted(int(path.name) for path in tasks if int(path.name) != pid)
                if len(workers) != owners:
                    raise ValueError("Expected one prepared worker per owner")
                command["worker_affinity"] = []
                for worker, cpu in zip(workers, cores):
                    before = sorted(os.sched_getaffinity(worker))
                    os.sched_setaffinity(worker, {cpu})
                    applied = sorted(os.sched_getaffinity(worker))
                    if applied != [cpu]:
                        raise ValueError("Worker affinity was not applied")
                    command["worker_affinity"].append({"tid": worker, "before": before, "measured": applied})
            command["before_go"] = cost_environment(cores)
            stdout, _ = child.communicate(b"GO\n", timeout=120)
            (out / "stdout.log").write_bytes(ready + stdout)
            command["after_exit"] = cost_environment(cores)
            if child.returncode:
                raise ValueError(f"Cost process exited {child.returncode}: {args[0]}")
        finally:
            if child.poll() is None:
                stop()
            command["exit"] = child.wait()
            save(out / "command.json", command)

    record = measurements(target)
    workload = (record["rate"], record["block"], record["instances"], record["warm_frames"], record["measure_frames"])
    if workload != (rate, block, owners, 2 * rate, rate):
        raise ValueError("Wrong rate, partition, owner count or duration in cost result")
    row = {"command": str(out / "command.json"), "table": str(target), "owners": [], "metrics": {
        "render_wall_ns": record["process_wall_seconds"] * 1e9,
        "render_cpu_ns": record["process_cpu_seconds"] * 1e9,
        "cold_prepare_ns": sum(record[f"{i}.load_seconds"] for i in range(owners)) * 1e9,
        "peak_rss_bytes": record["peak_rss_bytes"]}}
    if any(sample <= 0 for sample in row["metrics"].values()):
        raise ValueError("Missing cost metric")
    for index in range(owners):
        prefix = f"{index}."
        counters = {field: record[prefix + field] for field in
                    ("frames", "ticks", "native_frames", "measure_ticks", "measure_native_frames",
                     "crank_rad_s", "nonzero_frames")}
        if (counters["frames"] != 3 * rate or counters["native_frames"] != 3 * 44100
                or counters["measure_native_frames"] != 44100 or counters["measure_ticks"] <= 0
                or not 0 < counters["nonzero_frames"] <= rate):
            raise ValueError("Incomplete measured clocks or signal")
        outputs = [("", rate), (".previous", 1)]
        if transients:
            outputs.append((".warm", 2 * rate))
        evidence = {}
        for suffix, frames in outputs:
            raw = Path(f"{target}.{prefix}f32le{suffix}")
            if raw.stat().st_size != frames * 4:
                raise ValueError("Incomplete measured PCM")
            sha = digest(raw)
            saved = archive / f"{sha}.f32le"
            if saved.exists():
                if digest(saved) != sha:
                    raise ValueError("Archived PCM changed")
                raw.unlink()
            else:
                raw.rename(saved)
            evidence[suffix or "steady"] = sha
        row["owners"].append({"counters": counters, "pcm_sha256": evidence})
    save(out / "measurement.json", row)
    return row


def frozen_cases(package, original, reference):
    seal_path = package / "reference/r25-001-baseline-01/baseline-seal.json"
    seal = json.loads(seal_path.read_bytes())
    original_sha = digest(original)
    if seal["result"] != "pass" or seal["original_binaries"]["cost"]["sha256"] != original_sha:
        raise ValueError("Original executable does not match frozen cost evidence")
    revision = subprocess.check_output(["git", "-C", str(reference), "rev-parse", "HEAD"], text=True).strip()
    changes = subprocess.check_output(["git", "-C", str(reference), "--no-optional-locks", "status", "--porcelain"])
    if revision != seal["parent"] or changes:
        raise ValueError("Expected the clean frozen reference checkout")
    cached, selected = {}, {}
    for item in seal["performance_cell_sources"]:
        case = tuple(item["case"])
        path = package / item["path"]
        if path not in cached:
            if digest(path) != seal["artifacts"][item["path"]]:
                raise ValueError(f"Frozen cost budget changed: {path}")
            cache = json.loads(path.read_bytes())
            if cache["old_revision"] != revision or cache["binary_sha256"] != original_sha:
                raise ValueError("Frozen budget provenance mismatch")
            if platform.platform() != cache["platform"] or not set(cache["affinity"]).issubset(os.sched_getaffinity(0)):
                raise ValueError("Current host does not match the frozen cost tuple")
            for cpu, governor in cache["governors"].items():
                observed = Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor").read_text().strip()
                if governor != observed:
                    raise ValueError("CPU governor differs from frozen cost tuple")
            cached[path] = (cache, {tuple(cell["case"]): cell for cell in cache["cells"]})
        cache, cells = cached[path]
        cell = cells[case]
        if case in selected or cell["rows"] != 33 or not all(metric["holdout_pass"] for metric in cell["budgets"].values()):
            raise ValueError("Each frozen cell needs its own passing baseline-only holdout")
        selected[case] = (path, cache, cell)
    expected = set(itertools.product(("fixture", "kohler", "ls"), (44100, 48000),
                                      ("free", "driven"), (1, 256, 1024), (1, 2)))
    if set(selected) != expected or seal["performance_cells"] != len(expected):
        raise ValueError("Frozen cost matrix is incomplete")
    return seal_path, selected


def matched_costs(args, report, archive, env):
    seal, selected = frozen_cases(args.package, args.reference, args.reference_source)
    report["baseline_seal"] = {"path": str(seal), "sha256": digest(seal)}
    report["selected_cases"] = args.case or "all"
    cases = {case: data for case, data in selected.items()
             if not args.case or "-".join(map(str, case)) in args.case}
    if args.case and len(cases) != len(set(args.case)):
        raise ValueError("Unknown or missing cost matrix cell")
    source = Path(__file__).resolve().parents[1]
    old_fixture = args.reference_source / "examples/runtime/fixture"
    paths = {"kohler": "atg-video-1/02_kohler_ch750.json", "ls": "atg-video-2/07_gm_ls.json"}
    identities = {}
    for case, (budget, metadata, frozen) in cases.items():
        name = case[0]
        if name == "fixture":
            old_inputs = (old_fixture / "main.mr", old_fixture / "library", old_fixture)
            new_inputs = (args.fixture / "main.json", args.fixture)
        else:
            old_inputs = (budget.parent / f"{name}.mr", args.reference_source / "es", args.reference_source / "assets")
            new_inputs = (source / "assets/engines" / paths[name], args.resources)
        if digest(old_inputs[0]) != metadata["input_sha256"][name]:
            raise ValueError("Reference script entry differs from frozen workload")
        identities[str(old_inputs[0])] = digest(old_inputs[0])
        identities[str(new_inputs[0])] = digest(new_inputs[0])
        data = json.loads(new_inputs[0].read_bytes())
        for impulse in data["components"]["impulses"]:
            locator = Path(impulse["path"])
            original_locator = locator if name == "fixture" else Path(*locator.parts[1:])
            new_input = new_inputs[1] / locator
            old_input = old_inputs[2] / original_locator
            sha = digest(old_input)
            if digest(new_input) != sha:
                raise ValueError(f"Wave bytes differ between arms: {locator}")
            identities.update({str(old_input): sha, str(new_input): sha})
        case_name = "-".join(map(str, case))
        folder = args.out / case_name
        folder.mkdir()
        precondition = metadata["conditions"].get("machine_preconditioning_seconds", 0)
        if precondition:
            start = time.monotonic()
            repeat = 0
            while time.monotonic() - start < precondition:
                installed_trial(folder / f"precondition-{repeat:03d}", archive, args.reference,
                                old_inputs, case, env, metadata, launcher=args.launcher)
                repeat += 1
        result = {"case": case, "budget": str(budget), "budget_sha256": digest(budget), "pairs": []}
        arms = {"original": (args.reference, old_inputs), "installed": (args.installed, new_inputs)}
        for index in range(10):
            order = tuple(arms) if index % 2 == 0 else tuple(reversed(arms))
            rows = {}
            for arm in order:
                executable, inputs = arms[arm]
                rows[arm] = installed_trial(folder / f"{index:02d}-{arm}", archive, executable,
                                            inputs, case, env, metadata, launcher=args.launcher)
            for row in rows.values():
                if [owner["pcm_sha256"]["steady"] for owner in row["owners"]] != frozen["pcm_sha256"]:
                    raise ValueError(f"{case_name} PCM differs from the frozen one-second window")
            if rows["original"]["owners"] != rows["installed"]["owners"]:
                raise ValueError(f"{case_name} telemetry or preceding PCM differs")
            result["pairs"].append({"index": index, "order": order, "runs": rows})
            save(folder / "case.json", result)
        outcomes = {}
        for metric, limit in frozen["budgets"].items():
            original = [pair["runs"]["original"]["metrics"][metric] for pair in result["pairs"]]
            candidate = [pair["runs"]["installed"]["metrics"][metric] for pair in result["pairs"]]
            a, b, change = map(distribution, (original, candidate, [y - x for x, y in zip(original, candidate)]))
            verdict = "pass"
            if a["median"] > limit["upper"]:
                verdict = "inconclusive-reference-drift"
            elif b["median"] > limit["upper"] or change["median"] > limit["pair_margin"]:
                verdict = "fail"
            outcomes[metric] = {"original": a, "installed": b, "paired_increase": change,
                                "frozen": limit, "verdict": verdict}
        result["outcomes"] = outcomes
        save(folder / "case.json", result)
        report["cases"].append({"path": str(folder / "case.json"), "outcomes": outcomes})
        save(args.out / "run.json", report)
        print(case_name, {metric: outcome["verdict"] for metric, outcome in outcomes.items()}, flush=True)
    save(args.out / "input-identities.json", identities)
    if any(digest(Path(path)) != sha for path, sha in identities.items()):
        raise ValueError("Benchmark input changed during study")
    return all(outcome["verdict"] == "pass" for case in report["cases"] for outcome in case["outcomes"].values())


def installed_costs(arguments):
    parser = argparse.ArgumentParser(description="Measure an installed runtime against frozen R2 costs.")
    parser.add_argument("--out", type=absolute, required=True)
    parser.add_argument("--installed", type=absolute, required=True)
    parser.add_argument("--fixture", type=absolute, required=True)
    parser.add_argument("--resources", type=absolute)
    parser.add_argument("--reference", type=absolute)
    parser.add_argument("--reference-source", type=absolute)
    parser.add_argument("--package", type=absolute)
    parser.add_argument("--case", action="append")
    parser.add_argument("--launcher", type=absolute, help="Linux launcher isolating each benchmark process RSS")
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args(arguments)
    if args.out.is_relative_to(Path(__file__).resolve().parents[1]):
        parser.error("Output must be outside the source checkout")
    if not args.smoke and not all((args.resources, args.reference, args.reference_source, args.package, args.launcher)):
        parser.error("The complete study requires staged resources, frozen reference executable/source, package and RSS launcher")
    args.out.mkdir(parents=True)
    archive = args.out / "pcm"
    archive.mkdir()
    env = {key: value for key, value in os.environ.items() if key not in
           ("DISPLAY", "WAYLAND_DISPLAY", "PULSE_SERVER", "SDL_AUDIODRIVER", "LD_LIBRARY_PATH")}
    env.update(LC_ALL="C", TZ="UTC")
    files = [path for path in (Path(__file__).resolve(), args.installed, args.reference, args.launcher) if path]
    report = {"version": 1, "conditions": conditions(), "cases": [], "status": "running",
              "warm_seconds": 2, "measured_seconds": 1, "seeds": [403, 404],
              "filesystem": "warm; hash inputs before first preparation", "locale": "C", "timezone": "UTC",
              "argv": sys.argv, "identities": {str(path): digest(path) for path in files}}
    save(args.out / "run.json", report)
    try:
        if args.smoke:
            inputs = (args.fixture / "main.json", args.fixture)
            report["inputs"] = {str(inputs[0]): digest(inputs[0])}
            for rate, mode in itertools.product((44100, 48000), ("free", "driven")):
                rows = [installed_trial(args.out / f"{rate}-{mode}-{block}", archive, args.installed, inputs,
                                        ("fixture", rate, mode, block, 2), env, transients=True)
                        for block in (1, 256, 1024, 0)]
                if any(row["owners"] != rows[0]["owners"] for row in rows):
                    raise ValueError("Partition altered installed PCM or clocks")
                report["cases"].append({"rate": rate, "mode": mode, "rows": rows})
            passed = True
        else:
            passed = matched_costs(args, report, archive, env)
        report["status"] = "pass" if passed else "cost-limits-not-met"
    except BaseException as error:
        report.update(status="fail", reason=str(error))
        raise
    finally:
        save(args.out / "run.json", report)
    return passed


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
        models = [{"name": "fixture", "entry": args.fixture / "main.json", "assets": args.fixture}]
        if args.reference:
            models[0]["old"] = (args.fixture / "main.mr", args.fixture / "library", args.fixture)
        schedule = "fixture"
    else:
        models, schedule = [], "legacy"
        for name, module in (("KOHLER", "engines/atg-video-1/02_kohler_ch750"),
                             ("LS", "engines/atg-video-2/07_gm_ls")):
            model = {"name": name, "entry": args.resources / f"assets/{module}.json", "assets": args.resources}
            if args.reference:
                entry = inputs / (name + ".mr")
                entry.write_text(f'import "engine_sim.mr"\nimport "{module}.mr"\nmain()\n', encoding="utf-8")
                model["old"] = (entry, args.resources / "es", args.resources / "assets")
            models.append(model)
    environment = conditions()
    identities = set()
    for model in models:
        entry = model["entry"]
        identities.add(entry)
        document = json.loads(entry.read_bytes())
        identities.update(model["assets"] / ir["path"] for ir in document["components"]["impulses"])
        if args.reference:
            for place in model["old"]:
                if place.is_file():
                    identities.add(place)
                else:
                    identities.update(p.resolve() for p in place.rglob("*")
                                      if p.is_file() and p.suffix.lower() in (".mr", ".wav"))
    identities = sorted(identities)
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
                target = Path(conversion["measurement_files"][0] + ".0.f32le")
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
        if sys.argv[1:2] == ["installed-costs"]:
            sys.exit(0 if installed_costs(sys.argv[2:]) else 1)
        main()
    except (ValueError, OSError, subprocess.SubprocessError, TimeoutError, concurrent.futures.TimeoutError) as error:
        print(str(error), file=sys.stderr); sys.exit(1)
