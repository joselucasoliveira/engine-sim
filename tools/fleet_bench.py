#!/usr/bin/env python3
"""Run and retain Engine Simulator Fleet's native, device-neutral paced study."""

import argparse
import csv
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time


CALLBACK = struct.Struct("<QQQQQIIiI")
TILE = struct.Struct("<QQQQQ")
RATE = 48000
QUANTUM = 64
PROFILES = {"Interactive": (2, 128, 3), "Balanced": (1, 256, 4)}
SCREEN_BLOCKS = {"Interactive": (64, 128), "Balanced": (64, 128, 256, 512, 1024)}
LS_PATH = Path("assets/engines/atg-video-2/07_gm_ls.json")
LS_SHA256 = "f3d5f7958d1759cd7214b41373747b4304ca4cb7c441b3a5cd8973c31e4a44fe"
PRIVATE_INPUTS = {
    "examples/runtime/fixture/main.json": "b0d67dae7bab867eed3f79fd9aae581b3eabaac329e0a769c1f82a3408e12bfe",
    "assets/engines/atg-video-1/02_kohler_ch750.json": "da71ee45143e5d5dd10e6146fb63cb8fe66f57ce153edd911ae572ff6f873374",
    "examples/runtime/fixture/impulse.wav": "97fb8b13c2ed77769f297659db76203f86d90d56201e02df38b5a295a1bb336b",
    "es/sound-library/smooth/smooth_39.wav": "75de9db47063395665d36b6d4232f477aae385feaa9ba158353fbdaf122db5cc",
}


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as file:
        for part in iter(lambda: file.read(1024 * 1024), b""):
            result.update(part)
    return result.hexdigest()


def save(path, data):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def run(args, log, *, env=None, cwd=None, timeout=None, required=True):
    argv = [str(arg) for arg in args]
    with log.open("a", encoding="utf-8") as output:
        output.write(f"$ {shlex.join(argv)}\n")
        output.flush()
        try:
            result = subprocess.run(argv, cwd=cwd, env=env, stdout=output,
                                    stderr=subprocess.STDOUT, timeout=timeout, check=False)
        except subprocess.TimeoutExpired:
            output.write(f"timed out after {timeout} seconds\n")
            raise
        output.write(f"exit {result.returncode}\n")
    if required and result.returncode:
        raise RuntimeError(f"Command exited {result.returncode}. See {log}")
    return result.returncode


def counters(path):
    return {key: int(value) for key, value in (line.split() for line in path.read_text().splitlines())}


def table(path):
    with path.open(newline="", encoding="utf-8") as file:
        return list(csv.DictReader(file, delimiter="\t"))


def dist(values):
    ordered = sorted(values)
    if not ordered:
        return {"count": 0}
    n = len(ordered)
    percentiles = (50, 95, 99, 99.9) if n >= 10000 else (50, 95, 99)
    return {"count": n, "max": ordered[-1], "min": ordered[0],
            **{f"p{p}": ordered[math.ceil(n * p / 100) - 1] for p in percentiles}}


def check_voice_clocks(path, rate, frequencies):
    voices = table(path / "voices.tsv")
    activations = {(int(e["lane"]), int(e["next"])): int(e["applied"])
                   for e in table(path / "events.tsv") if e["reason"] == "0" and e["kind"] in ("1", "10")}
    previous = {}
    rpm = [set() for _ in frequencies]
    phase = [set() for _ in frequencies]
    coherent = bool(voices)
    all_samples = [*voices, *table(path / "retired.tsv")]
    for row in all_samples:
        lane = int(row["lane"])
        key = (lane, int(row["generation"]))
        elapsed = int(row["frames"])
        ticks = int(row["ticks"])
        native = int(row["native"])
        coherent &= (row["fault"] == "0" and ticks == elapsed * frequencies[lane] // rate and
                     native == (elapsed * 44100 + rate - 1) // rate)
        if "G" not in row:
            continue
        generated = int(row["G"])
        coherent &= key in activations and elapsed == generated - activations.get(key, generated)
        coherent &= generated > previous.get(key, -1)
        previous[key] = generated
        readings = [float.fromhex(row[field]) for field in ("rpm", "phase", "demand", "load", "target_rpm")]
        coherent &= all(math.isfinite(value) for value in readings)
        rpm[lane].add(readings[0])
        phase[lane].add(readings[1])
    moving = all(len(values) > 1 and max(values) > 0 for values in rpm) and all(len(values) > 1 for values in phase)
    return {"model_clock_counts": coherent and set(previous) == set(activations), "observed_voice_motion": moving}, len(voices)


def collect(path, rate, block, high, frames, live, observed, *, paced=True, variable=False, overlaps=0, roster="FD"):
    data = counters(path / "counts.tsv")
    callbacks = list(CALLBACK.iter_unpack((path / "callbacks.bin").read_bytes()))
    windows = table(path / "windows.tsv")
    lanes = range(len(roster))
    windows = [(int(row["frames"]), *(int(row[f"nonzero{i}"]) for i in lanes)) for row in windows]
    events = table(path / "events.tsv")
    nonzero = [sum(row[i + 1] > 0 for row in windows[1:] if row[0]) for i in lanes]
    bad_windows = sum(any(x == 0 for x in row[1:]) or row[0] != min(rate, frames - i * rate)
                      for i, row in enumerate(windows) if i > 0)
    consumer, wake, host = [], [], []
    expected = 0
    callback_invalid = deadline_misses = fallback = 0
    counts = []
    for index, (first, due, enter, leave, done, pcm, missed, status, _) in enumerate(callbacks):
        nominal = (block, block // 2, 3 * block // 4)[index % 3] if variable else block
        count = min(nominal, frames - expected)
        counts.append(count)
        if (first != expected or pcm + missed != count or enter > leave or leave > done or
                (paced and enter < due) or min(pcm, missed) < 0 or (status == 0 and missed != 0)):
            callback_invalid += 1
        expected += count
        fallback += missed
        consumer.append(leave - enter)
        wake.append(enter - due)
        host.append(done - leave)
        deadline_misses += paced and bool(callbacks) and done > callbacks[0][1] + expected * 10**9 // rate
    criteria = {
        "each_call_headroom": not paced or all(ns * 4 * rate <= count * 10**9 for ns, count in zip(consumer, counts)),
        "callback_frame_accounting": bool(callbacks) and not callback_invalid and data["frames"] == frames == expected,
        "absolute_deadlines": not paced or not deadline_misses,
        "native_and_parsed_deadlines_agree": data["deadline_misses"] == deadline_misses,
        "transport": not any(data[k] for k in ("transport_breaks", "fallback_frames", "queue_rejections")) and not fallback,
        "stem_progress": not bad_windows and min(nonzero) > 0 and all(data[f"ticks{i}"] > 0 for i in lanes),
        "finite_pcm": data["invalid_samples"] == 0,
        "ordered_event_admission": not data["late_events"] and not data["unsubmitted_events"] and
                                   all(int(e["submit"]) == int(e["reason"]) == 0 and
                                       int(e["frame"]) == int(e["applied"]) for e in events),
        "fault_free": data["unexpected_faults"] == 0,
        "overlaps_completed": data["overlap_completions"] >= overlaps,
    }
    tile_time, control_to_publish, control_to_return, control_start, control_reaped, seals = [], [], [], [], [], []
    tiles = []
    full_window = (rate + QUANTUM - 1) // QUANTUM
    largest_window = 0
    if observed and (path / "tiles.bin").is_file():
        tiles = list(TILE.iter_unpack((path / "tiles.bin").read_bytes()))
        for i, (first, submitted, begin, published, end) in enumerate(tiles):
            seals.append(first + QUANTUM - submitted)
            tile_time.append(end - begin)
        window = sum(tile_time[:full_window])
        if len(tile_time) >= full_window:
            largest_window = window
            for remove, add in zip(tile_time, tile_time[full_window:]):
                window += add - remove
                largest_window = max(largest_window, window)
        criteria.update({
            "all_tile_publications": len(tiles) * QUANTUM >= frames and all(
                f == i * QUANTUM and d <= f and 0 < b <= p <= e
                for i, (f, d, b, p, e) in enumerate(tiles)),
            "producer_sliding_reserve": 0 < largest_window * 4 * rate <= 3 * full_window * QUANTUM * 10**9,
            "bounded_sealed_lead": bool(seals) and 0 < max(seals) <= high,
        })
    elif observed:
        criteria["all_tile_publications"] = False
    if live:
        for event in events:
            sampled = int(event["sampled"])
            frame = int(event["frame"])
            if not sampled or event["reason"] != "0" or frame // QUANTUM >= len(tiles) or frame // block >= len(callbacks):
                continue
            control_start.append(sampled - int(event["trigger"]))
            control_to_publish.append(tiles[frame // QUANTUM][3] - sampled)
            control_to_return.append(callbacks[frame // block][3] - sampled)
            control_reaped.append(int(event["reaped"]) - sampled)
        game_updates = len(table(path / "game.tsv"))
        required_updates = (frames * 60 + rate - 1) // rate
        bound = (high + QUANTUM + block) * 10**9
        criteria.update({
            "game_updates_sampled": game_updates == required_updates,
            "fresh_control_lookahead": data["max_lookahead_frames"] == high + QUANTUM,
            "control_publication_age": bool(control_to_publish) and
                                       all(0 < age * rate <= bound for age in control_to_publish),
            "control_return_age": bool(control_to_return) and
                                  all(0 < age * rate <= bound for age in control_to_return),
        })
    stats = {
        **data, "callbacks": len(callbacks), "tiles": len(tiles), "cold_first_included": bool(callbacks and callbacks[0][0] == 0),
        "max_consumer_ns": max(consumer, default=0), "max_active_ns_per_750_tiles": largest_window,
        "active_tile_window": full_window, "max_sealed_lead_frames": max(seals, default=0),
        "bad_stem_windows": bad_windows, "nonzero_windows": nonzero,
        "live_events": len(control_to_return), "max_control_to_publish_ns": max(control_to_publish, default=0),
        "max_control_to_submit_ns": max(control_to_return, default=0),
        "first_callback_ns": consumer[0] if consumer else None, "first_tile_ns": tile_time[0] if tile_time else None,
        "distribution_ns": dict(consumer=dist(consumer), wake_late=dist(wake), host_work=dist(host),
                                producer=dist(tile_time), control_to_publish=dist(control_to_publish),
                                control_to_callback_return=dist(control_to_return), game_trigger_to_sample=dist(control_start),
                                control_to_reap=dist(control_reaped)),
        "criteria": criteria,
    }
    if (path / "tiles.bin.tsv").is_file():
        stats.update(counters(path / "tiles.bin.tsv"))
    return stats


def native_trial(runner, out, models, block, rate, profile, seconds, workload, cores, variant="normal", observed=True,
                 *, roster="FD", ls=None):
    out.mkdir()
    trial_keys = ("ES_FLEET_TRACE_FILE", "ES_FLEET_TRACE_TILES", "ES_FLEET_WORKER_CORE", "ES_FLEET_COORDINATOR_CORE",
                  "ES_FLEET_BENCH_ROSTER", "ES_FLEET_BENCH_LS_MODEL")
    env = {k: v for k, v in os.environ.items() if k not in trial_keys}
    env["ES_FLEET_BENCH_ROSTER"] = roster
    if ls is not None:
        env["ES_FLEET_BENCH_LS_MODEL"] = str(ls)
    if observed:
        env.update(ES_FLEET_TRACE_FILE=str(out / "tiles.bin"),
                   ES_FLEET_TRACE_TILES=str((rate * seconds + 16 * 1024) // QUANTUM))
        if cores is not None:
            env["ES_FLEET_WORKER_CORE"] = str(cores[1])
    audio, load = (-1, -1) if cores is None else (cores[0], cores[2])
    cmd = [runner, out, *models, block, rate, PROFILES[profile][0], rate * seconds, workload, audio, load, variant]
    if cores is not None and len(cores) > 3:
        env["ES_FLEET_COORDINATOR_CORE"] = str(cores[3])
    rc = run(cmd, out / "process.log", env=env, timeout=seconds + 120, required=False)
    if not (out / "counts.tsv").is_file():
        raise RuntimeError(f"No native counters (exit {rc}). See {out / 'process.log'}")
    row = collect(out, rate, block, block * PROFILES[profile][2], rate * seconds, workload == "live-loaded", observed,
                  paced=variant not in ("unp", "replay-smoke"), variable=variant == "variable",
                  overlaps=1 if variant == "replay-smoke" else seconds // 60 if "F" in roster else 0, roster=roster)
    input_models = {"F": models[0], "D": models[2], "L": ls}
    frequencies = [int(json.loads(input_models[kind].read_text())["engine"]["simulation_frequency"]) for kind in roster]
    checks, voice_samples = check_voice_clocks(out, rate, frequencies)
    row["criteria"].update(checks)
    row["voice_snapshot_count"] = voice_samples
    row["mix_peak"] = struct.unpack("<f", struct.pack("<I", row["mix_peak_float_bits"]))[0]
    row.update(profile=profile, workload=workload, rate=rate, block=block, elapsed_target_s=seconds, native_exit=rc,
               roster=roster)
    row["passed"] = not rc and all(row["criteria"].values())
    save(out / "metrics.json", row)
    return row


def replay_stems(executable, out, models, row, *, events=None, name="replay"):
    actual = out / f"{name}.tsv"
    timeline = events if events is not None else out / "events.tsv"
    run([executable, timeline, actual, *models, row["rate"], row["frames"]],
        out / f"{name}.log", timeout=row["elapsed_target_s"] + 120)
    original = table(out / "digests.tsv")
    isolated = table(actual)
    if len(original) != len(isolated):
        raise RuntimeError(f"Replay window count differs: {out}")
    differences = [entry for entry, found in zip(original, isolated) if entry != found]
    outcome = {"match": not differences, "compared_digests": len(original), "different_digests": len(differences),
               "first_difference": differences[0] if differences else None}
    save(out / f"{name}.json", outcome)
    return outcome


def self_check(args):
    args.output.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="smoke-", dir=args.output))
    light = args.source / "examples/runtime/fixture"
    heavy = args.source / "examples/fleet/fixture-heavy"
    models = [light / "main.json", light, heavy / "main.json", heavy]
    for variant in ("replay-smoke", "miss", "starve"):
        row = native_trial(args.runner, out / variant, models, 128, RATE, "Interactive", 2,
                           "scheduled", None, variant, observed=False)
        if variant == "replay-smoke":
            case = out / variant
            if not row["passed"] or not replay_stems(args.replay, case, models, row)["match"]:
                raise RuntimeError(f"Dual-engine smoke or independent replay failed. See {case}")
            actions = table(case / "events.tsv")
            next(event for event in actions if event["kind"] == "3")["gain"] = "0x0p+0"
            modified = case / "replay-negative-events.tsv"
            with modified.open("w", newline="", encoding="utf-8") as file:
                writer = csv.DictWriter(file, fieldnames=actions[0], delimiter="\t", lineterminator="\n")
                writer.writeheader()
                writer.writerows(actions)
            if replay_stems(args.replay, case, models, row, events=modified, name="replay-negative")["match"]:
                raise RuntimeError(f"Changed gain was not detected by replay. See {case}")
        if variant == "miss" and (row["passed"] or row["native_exit"] != 1 or row["deadline_misses"] != 1):
            raise RuntimeError("Injected host deadline miss was not rejected")
        if variant == "starve" and (row["passed"] or row["native_exit"] != 1 or row["transport_breaks"] != 1 or not row["fallback_frames"]):
            raise RuntimeError("Injected underflow was not rejected")
    if run([args.latency, "--inject-shift"], out / "latency-negative.log", required=False) != 1:
        raise RuntimeError("Injected marker shift was not rejected")
    print(f"Device-neutral functional smoke passed. No sustained row emitted. Evidence {out}")


def read_text(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def select_cores(requested):
    if not hasattr(os, "sched_getaffinity"):
        if requested:
            raise RuntimeError("Explicit core affinity is unavailable on this platform")
        return None
    allowed = sorted(os.sched_getaffinity(0))
    root = Path("/sys/devices/system/cpu")
    topology = {}
    for cpu in allowed:
        info = root / f"cpu{cpu}"
        same_core = read_text(info / "topology/thread_siblings_list")
        frequency = read_text(info / "cpufreq/cpuinfo_max_freq")
        topology[cpu] = (same_core or str(cpu), frequency)
    if requested is not None:
        cores = tuple(int(value) for value in requested.split(","))
        if len(cores) not in (3, 4) or len(set(cores)) != len(cores) or any(c not in topology for c in cores):
            raise RuntimeError("Require distinct allowed CPU IDs for A,W,load[,C]")
        if len({topology[c][0] for c in cores[:3]}) != 3:
            raise RuntimeError("A, W and load must occupy distinct physical cores")
        return cores
    classes = {}
    for cpu in allowed:
        siblings, freq = topology[cpu]
        classes.setdefault(int(freq or 0), {}).setdefault(siblings, cpu)
    for clock in sorted(classes, reverse=True):
        candidates = sorted((c for c in classes[clock].values() if c != 0), reverse=True)
        if len(candidates) >= 3:
            cores = candidates[:3]
            other = next((c for c in allowed if c and topology[c][0] not in {topology[i][0] for i in cores}), None)
            return (*cores, other) if other is not None else tuple(cores)
    raise RuntimeError("No three distinct allowed physical cores of one recorded frequency class. Provide the documented selection.")


def conditions(cores):
    cpu = Path("/sys/devices/system/cpu")
    return {
        "os": platform.platform(), "machine": platform.machine(), "host": "native-device-neutral",
        "hardware_clock": str(time.get_clock_info("monotonic")), "backend": "absolute paced host (no DAC)",
        "cores_A_W_load_C": cores, "cgroup": read_text("/proc/self/cgroup"),
        "kernel_command_line": read_text("/proc/cmdline"), "isolated_cpus": read_text(cpu / "isolated"),
        "cpuinfo": read_text("/proc/cpuinfo"), "scheduling_policy": os.sched_getscheduler(0) if hasattr(os, "sched_getscheduler") else None,
        "core_power": {c: {name: read_text(cpu / f"cpu{c}/cpufreq/{name}") for name in
                           ("scaling_driver", "scaling_governor", "scaling_min_freq", "scaling_max_freq", "scaling_cur_freq")}
                       for c in cores or ()},
        "rounding": "recorded by A and W", "physical_stream_and_loopback": "pending independent endpoint evidence",
    }


def installed_runner(args, root, linkage, observed, out):
    build = root / "sdk-build"
    prefix = root / "install"
    host = root / "host-build"
    root.mkdir(parents=True)
    log = out / f"build-{linkage}{'-control' if not observed else ''}.log"
    cache = {}
    for text in (args.build / "CMakeCache.txt").read_text().splitlines():
        if not text.startswith(("#", "//")) and ":" in text.split("=", 1)[0] and "=" in text:
            key, value = text.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    generator = ["-G", cache["CMAKE_GENERATOR"]]
    for key, flag in (("CMAKE_GENERATOR_PLATFORM", "-A"), ("CMAKE_GENERATOR_TOOLSET", "-T")):
        if cache.get(key):
            generator.extend((flag, cache[key]))
    cmake = cache.get("CMAKE_COMMAND", "cmake")
    toolchain = [f"-D{k}={cache[k]}" for k in ("CMAKE_CXX_COMPILER", "CMAKE_MAKE_PROGRAM") if cache.get(k)]
    configure = [cmake, "-S", args.source, "-B", build, *generator, "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF",
                 "-DENGINE_SIM_BUILD_RUNTIME=ON", "-DENGINE_SIM_BUILD_DESKTOP=OFF", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                 f"-DENGINE_SIM_RUNTIME_SHARED={'ON' if linkage == 'shared' else 'OFF'}",
                 f"-DENGINE_SIM_FLEET_BENCH_OBSERVE={'ON' if observed else 'OFF'}", *toolchain]
    configure += [f"-D{k}={cache[k]}" for k in ("ENGINE_SIM_DEPS_FILE", "ENGINE_SIM_PRESET_ROOT") if cache.get(k)]
    run(configure, log)
    run([cmake, "--build", build, "--config", "Release", "--target", "engine-sim-fleet",
         "--parallel", os.environ.get("R3_JOBS", "4")], log)
    run([cmake, "--install", build, "--config", "Release", "--prefix", prefix, "--component", "FleetSDK"], log)
    run([cmake, "-S", args.source / "test/runtime/fleet-bench", "-B", host, *generator, "-DCMAKE_BUILD_TYPE=Release",
         *toolchain, f"-DCMAKE_PREFIX_PATH={prefix}"], log)
    run([cmake, "--build", host, "--config", "Release"], log)
    def executable(name):
        result = next((p for p in host.rglob(name + "*") if p.is_file() and p.name in (name, name + ".exe")), None)
        if result is None:
            raise RuntimeError(f"Installed consumer {name} missing in {host}")
        return result
    runner = executable("fleet-paced")
    replay = executable("fleet-stem-replay")
    if (prefix / "bin").is_dir():
        for dll in (prefix / "bin").glob("*.dll"):
            shutil.copy2(dll, runner.parent)
    if (build / "compile_commands.json").is_file():
        shutil.copy2(build / "compile_commands.json", out / f"{log.stem}-compile.json")
    loaded = {str(p.relative_to(prefix)): digest(p) for p in sorted(prefix.rglob("*")) if p.is_file() and not p.is_symlink()}
    return runner, {"runner": str(runner), "runner_sha256": digest(runner), "installed_digests": loaded,
                    "replay": str(replay), "replay_sha256": digest(replay),
                    "cmake": cmake, "compiler": cache["CMAKE_CXX_COMPILER"], "observed": observed}


def study(args):
    if args.output.is_relative_to(args.source) or args.build.is_relative_to(args.source):
        raise RuntimeError("Use an external build and evidence directory")
    args.output.mkdir(parents=True)
    summary = args.output / "summary.json"
    identified = {name: digest(args.models / name) for name in PRIVATE_INPUTS}
    for name in identified:
        if identified[name] != PRIVATE_INPUTS[name]:
            raise RuntimeError(f"Frozen model or impulse identity mismatch: {args.models / name}")
    ls = args.models / LS_PATH
    if args.screen_only:
        if digest(ls) != LS_SHA256:
            raise RuntimeError(f"Frozen GM LS identity mismatch: {ls}")
        identified[str(LS_PATH)] = LS_SHA256
    paths = list(identified)
    single, driven = args.models / paths[0], args.models / paths[1]
    models = [single, single.parent, driven, args.models]
    if subprocess.run(["git", "diff", "--quiet"], cwd=args.source, check=False).returncode:
        raise RuntimeError("Load the exact staged candidate or a clean committed source tree")
    if subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard"], cwd=args.source):
        raise RuntimeError("Untracked sources prevent an exact candidate tree measurement")
    tree = subprocess.check_output(["git", "write-tree"], cwd=args.source, text=True).strip()
    cores = select_cores(args.cores)
    build_root = Path(tempfile.mkdtemp(prefix="fleet-native-", dir=args.build.parent))
    book = {"protocol": "r3-v1", "tree": tree, "tuple": conditions(cores), "models": [identified[name] for name in paths[:2]],
            "input_sha256": identified, "sdk_builds": {}, "controls": {}, "screens": [], "roster_screens": [],
            "paced_runs": [], "status": "incomplete"}
    save(summary, book)
    runners = {}
    for linkage in ("static", "shared"):
        runner, compiled = installed_runner(args, build_root / linkage, linkage, True, args.output)
        control, control_build = installed_runner(args, build_root / (linkage + "-control"), linkage, False, args.output)
        book["sdk_builds"][linkage] = compiled
        book["sdk_builds"][linkage + "-control"] = control_build
        runners[linkage] = runner
        comparison = []
        for executable, trace, label in ((runner, True, "observed"), (control, False, "control")):
            comparison.append(native_trial(executable, args.output / f"{linkage}-pcm-{label}", models,
                                           128, RATE, "Interactive", 2, "scheduled", None, "unp", trace))
        left, right = comparison
        identical = all(row["native_exit"] == 0 for row in comparison) and all(left[k] == right[k] for k in ("stem0", "stem1", "frames"))
        book["controls"][linkage] = {"frames": left["frames"], "stems": [left["stem0"], left["stem1"]], "pcm_digest_match": identical}
        if not identical:
            book["status"] = "failed"
            save(summary, book)
            raise RuntimeError(f"Worker observation changed the exact paired {linkage} stem digests")
        save(summary, book)
    if args.screen_only:
        for linkage, runner in runners.items():
            for rate, profile in itertools.product((44100, 48000), PROFILES):
                for block, variant in itertools.product(SCREEN_BLOCKS[profile], ("normal", "variable")):
                    label = f"screen-{linkage}-{rate}-{profile.lower()}-{block}-{variant}"
                    print(f"Starting {label}: 60s", flush=True)
                    row = native_trial(runner, args.output / label, models, block, rate, profile, 60, "scheduled", cores, variant)
                    book["screens"].append({"linkage": linkage, "variant": variant, "path": label, **row})
                    save(summary, book)
                block = PROFILES[profile][1]
                families = {
                    "fixture": ["F" * n for n in range(1, 9)],
                    "kohler": ["D", *("FD" + "F" * n for n in range(7))],
                    "ls": ["L", "DL", *("FDL" + "F" * n for n in range(6))],
                }
                for family, rosters in families.items():
                    for roster in rosters:
                        label = f"roster-{linkage}-{rate}-{profile.lower()}-{family}-{roster.lower()}"
                        print(f"Starting {label}: 60s", flush=True)
                        try:
                            row = native_trial(runner, args.output / label, models, block, rate, profile, 60, "scheduled", cores,
                                               roster=roster, ls=ls)
                        except RuntimeError as problem:
                            row = {"passed": False, "roster": roster, "rate": rate, "block": block, "profile": profile,
                                   "error": str(problem)}
                        book["roster_screens"].append({"linkage": linkage, "family": family, "path": label, **row})
                        save(summary, book)
                        if not row["passed"]:
                            break
        book["status"] = "screening-only" if all(row["passed"] for row in book["screens"]) else "screening-failed"
        save(summary, book)
        if book["status"] == "screening-failed":
            raise RuntimeError(f"One or more 60s screens failed. See {summary}")
        return
    for linkage, runner in runners.items():
        for profile, workload in itertools.product(PROFILES, ("live-loaded", "scheduled")):
            block = PROFILES[profile][1]
            for trial in range(1, args.repeats + 1):
                label = f"{linkage}-{profile.lower()}-{workload}-{trial}"
                out = args.output / label
                print(f"Starting {label}: {args.seconds}s", flush=True)
                row = native_trial(runner, out, models, block, RATE, profile, args.seconds, workload, cores)
                if row["passed"]:
                    replay = Path(book["sdk_builds"][linkage + "-control"]["replay"])
                    outcome = replay_stems(replay, out, models, row)
                    row["criteria"]["independent_voice_pcm"] = outcome["match"]
                    row["passed"] = outcome["match"]
                row.update(linkage=linkage, trial=trial)
                row["artifacts"] = [{"path": str(p.relative_to(args.output)), "sha256": digest(p)}
                                    for p in sorted(out.iterdir()) if p.is_file() and p.stat().st_size]
                book["paced_runs"].append(row)
                if not row["passed"]:
                    book["status"] = "failed"
                    save(summary, book)
                    invalid = ", ".join(k for k, value in row["criteria"].items() if not value)
                    raise RuntimeError(f"{label} failed ({invalid}). Partial study preserved in {summary}")
                save(summary, book)
    book["status"] = "passed"
    save(summary, book)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-check", action="store_true")
    parser.add_argument("--screen-only", action="store_true", help="run 60s rate, callback size and roster screens")
    parser.add_argument("--profile-set", choices=["r3-v1"])
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--models", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runner", type=Path)
    parser.add_argument("--latency", type=Path)
    parser.add_argument("--replay", type=Path)
    parser.add_argument("--seconds", type=int, default=600)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--cores", default=os.environ.get("ES_FLEET_BENCH_CORES"), help="A,W,load[,C] physical CPU IDs on Linux")
    args = parser.parse_args()
    for key, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, key, value.resolve())
    try:
        if args.self_check:
            if not args.runner or not args.latency or not args.replay:
                parser.error("--self-check requires --runner, --latency and --replay")
            self_check(args)
        else:
            if not args.profile_set or not args.build or not args.models or (args.seconds, args.repeats) != (600, 3):
                parser.error("The sustained r3-v1 matrix requires --profile-set r3-v1, --build, --models and three 600s repeats")
            study(args)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as problem:
        print(problem, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
