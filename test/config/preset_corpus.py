"""Check an explicit preset index with the real writer and both runtime paths."""
import argparse
import json
import math
import re
import struct
import subprocess
import tempfile
from pathlib import Path


HEADER = "frame\tdemand\tload_nm\tignition\tstarter\tdriven_rpm\n"
STANDARD = HEADER + "0\t0.2\t0\t1\t1\t0\n8000\t0.5\t0\t1\t0\t0\n"
PROFILES = ("all-entry", "deep")
FRAMES = 24000


def deep_controls(mode, rate, target):
    if mode == "free":
        controls = [(0, .18, 0, 1, 1, 0), (rate//5, .28, 0, 1, 0, 0),
                    (rate//2, .55, 0, 1, 0, 0), (rate*4//5, .52, 3, 1, 0, 0),
                    (rate*6//5, .1, 0, 0, 0, 0), (rate*3//2, .22, 0, 1, 1, 0),
                    (rate*9//5, .38, 0, 1, 0, 0)]
    else:
        controls = []
        for step in range(200):
            if step < 5:
                rpm = 0
            elif step < 20 or step >= 175:
                rpm = 600
            elif step < 75:
                rpm = 600 + (target - 600) * (step - 20) / 55
            elif step < 130:
                rpm = target
            elif step < 160:
                rpm = target * (160 - step) / 30
            else:
                rpm = 0
            controls.append((rate*step//100, .45 if step < 130 else .16,
                             10 if 80 <= step < 115 else 0, int(not 115 <= step < 150), 0, rpm))
    return HEADER + "".join("\t".join(map(str, row)) + "\n" for row in controls)


def render_cases(item):
    yield "sealed", "free", 44100, FRAMES, STANDARD
    if item["coverage"] == "deep":
        target = item.get("deep_drive_rpm", 1800)
        require(type(target) is int and 0 < target <= 12000, "Invalid deep drive range")
        for mode in ("free", "driven"):
            for rate in (44100, 48000):
                yield "deep", mode, rate, 2 * rate, deep_controls(mode, rate, target)


def require(condition, detail):
    if not condition:
        raise ValueError(detail)


def run(argv, work, expected=0):
    result = subprocess.run(list(map(str, argv)), cwd=work, capture_output=True)
    message = result.stdout + result.stderr
    require(result.returncode == expected,
            f"{argv[0]} failed with {result.returncode}: {message.decode(errors='replace')}")
    return message


def indexed(root, filename, count):
    index = json.loads(filename.read_bytes())
    require(index["version"] == 1 and len(index["entries"]) == count, "Unexpected preset denominator")
    paths = set()
    for item in index["entries"]:
        source, model = (root / item["source"], root / item["model"])
        require(source.resolve().is_relative_to(root) and model.resolve().is_relative_to(root), "Preset escapes checkout")
        require(source.suffix == ".mr" and source.is_file() and model == source.with_suffix(".json")
                and model.is_file() and source not in paths, "Missing, incorrect or duplicate preset")
        selection = item["selection"]
        require(item["coverage"] in PROFILES and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", selection) is not None,
                "Unsupported preset selection or profile")
        paths.add(source)
        yield item, source, model


def coverage(args, work):
    visited = []
    for item, source, model in indexed(args.repo, args.index, args.count):
        label = source.name
        output = work / model.name
        run([args.writer, model, output], work)
        require(model.read_bytes() == output.read_bytes(), f"{label}: writer emitted different canonical bytes")
        graph = json.loads(output.read_bytes())
        require(graph["selected_engine"] == graph["engine"]["id"] and graph["engine"]["banks"],
                f"{label}: missing selected engine topology")
        for response in graph["components"]["impulses"]:
            path = (args.repo / response["path"]).resolve()
            require(path.is_relative_to(args.repo) and path.is_file(), f"{label}: missing IR {response['path']}")
        visited.append(source.relative_to(args.repo))

    if args.numbered:
        numbers = range(args.first_number, args.first_number + args.count)
        def numbered(suffix):
            return sorted(path.relative_to(args.repo) for path in args.index.parent.glob(f"[0-9][0-9]_*{suffix}")
                          if int(path.name[:2]) in numbers)
        declarations = numbered(".mr")
        require([int(path.name[:2]) for path in declarations] == list(numbers) and visited == declarations,
                "Numbered source order or helper exclusion differs")
        require(numbered(".json") == [source.with_suffix(".json") for source in declarations],
                "Numbered preset coverage differs")
    require(len(visited) == args.count, "Incomplete preset coverage")


def legacy_entry(args, source, item, work):
    if item["selection"] == "entry":
        return source
    declared = source.relative_to(args.repo / "assets").as_posix()
    call = "main()" if item["selection"] == "main" else f"set_engine({item['selection']}())"
    path = work / (source.stem + "-entry.mr")
    path.write_text(f'import "engine_sim.mr"\nimport "{declared}"\n{call}\n', encoding="utf-8")
    return path


def render_flags(model, root, mode, rate, frames, schedule, output):
    return ["--entry", model, "--asset-root", root, "--mode", mode, "--rate", rate, "--seed", 403,
            "--frames", frames, "--schedule", schedule, "--out", output]


def behavior(args, work):
    comparisons = json.loads(args.index.read_bytes()).get("comparisons", [])
    paired = {row[side] for row in comparisons for side in ("left", "right")}
    traces = {}
    for index, (item, source, model) in enumerate(indexed(args.repo, args.index, args.count)):
        entry = legacy_entry(args, source, item, work)
        if item["model"] in paired:
            target = work / f"trace-{index}"
            run([args.probe, args.repo, entry, model, target], work)
            traces[item["model"]] = target
        for flavor, mode, rate, frames, controls in render_cases(item):
            scenario = f"{index}-{flavor}-{mode}-{rate}"
            legacy = work / (scenario + "-legacy.wav")
            canonical = work / (scenario + "-canonical.wav")
            schedule = work / (scenario + ".tsv")
            schedule.write_text(controls, encoding="ascii")
            try:
                old = run([args.legacy, "--library-root", args.repo / "es",
                           *render_flags(entry, args.repo / "assets", mode, rate, frames, schedule, legacy)], work)
                new = run([args.renderer, *render_flags(model, args.repo, mode, rate, frames, schedule, canonical)], work)
            except ValueError as error:
                raise ValueError(f"{source.name}: {flavor} {mode} {rate}: {error}") from error
            raw = canonical.read_bytes()
            require(raw == legacy.read_bytes() and old == new,
                    f"{source.name}: exact PCM or console mismatch at {flavor} {mode} {rate}")
            require(len(raw) == 58 + frames * 4, f"{source.name}: incomplete output")
            samples = struct.unpack_from(f"<{frames}f", raw, 58)
            require(all(map(math.isfinite, samples)) and any(value != 0 for value in samples),
                    f"{source.name}: missing finite nonzero acoustic output")

    fields = {"headers": ("routes", "endpoints"), "ignition": ("firing", "combustion")}
    for pair in comparisons:
        left, right = (traces[pair[side]] for side in ("left", "right"))
        for field in fields[pair["kind"]]:
            require((left / field).read_bytes() != (right / field).read_bytes(),
                    f"Expected distinct {field} for {pair['left']} and {pair['right']}")

    first = next(indexed(args.repo, args.index, args.count))[2]
    missing_root = work / "no-impulses"
    missing_root.mkdir()
    schedule = work / "rejected.tsv"
    schedule.write_text(STANDARD, encoding="ascii")
    output = work / "rejected.wav"
    missing = run([args.renderer, *render_flags(first, missing_root, "free", 44100, FRAMES, schedule, output)], work, 1)
    require(b"CFG_RESOURCE_PATH" in missing and not output.exists(), f"{first.name}: missing IR published output")
    truncated = work / "partial.json"
    truncated.write_bytes(first.read_bytes()[:first.stat().st_size // 2])
    invalid = run([args.renderer, *render_flags(truncated, args.repo, "free", 44100, FRAMES, schedule, output)], work, 1)
    require(b"CFG_SYNTAX" in invalid and not output.exists(), f"{first.name}: partial preset published output")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("check", choices=("coverage", "behavior"))
    parser.add_argument("repo", type=Path)
    parser.add_argument("index", type=Path)
    parser.add_argument("count", type=int)
    parser.add_argument("--numbered", action="store_true")
    parser.add_argument("--first-number", type=int, default=1)
    parser.add_argument("--writer", type=Path)
    parser.add_argument("--legacy", type=Path)
    parser.add_argument("--renderer", type=Path)
    parser.add_argument("--probe", type=Path)
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="engine-sim-presets-") as directory:
        (coverage if options.check == "coverage" else behavior)(options, Path(directory))
