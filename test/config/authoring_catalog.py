"""Build v2 candidates from evaluated .mr inputs and apply the v1 parity gates."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
from pathlib import Path
import re
import shlex
import struct
import subprocess
import tempfile

from preset_corpus import require, render_cases, render_flags

from authoring_sources import DEGREE, THOU, NUMBER, bits, fuel_curve, port_tables, source_points, sources


def author_number(value, unit):
    """Find a concise authoring number whose original conversion is exact."""
    nominal = value / unit
    for digits in range(1, 18):
        number = float(format(nominal, f".{digits}g"))
        if bits(number * unit) == bits(value):
            return int(number) if number.is_integer() else number
    raise ValueError(f"Cannot reproduce the evaluated parameter {value} in the authoring unit")


def authored_lift(value, primary, imports):
    for source in (primary, *imports):
        source_text = re.sub(r"//[^\n]*", "", source.read_text(encoding="utf-8"))
        matches = {(float(number), unit) for number, unit in re.findall(
            rf"\blift:\s*({NUMBER})\s*\*\s*units\.(mm|thou)\b", source_text)
                   if bits(float(number) * (0.001 if unit == "mm" else THOU)) == bits(value)}
        if len(matches) == 1:
            number, unit = matches.pop()
            return f"max_lift_{unit}", int(number) if number.is_integer() else number
        if len(matches) > 1:
            raise ValueError(f"Ambiguous authored lift unit in {source}")
    return "max_lift_thou", author_number(value, THOU)


def entries(repo):
    catalog = json.loads((repo / "assets/preset-catalog.json").read_bytes())
    rows = [item for index in catalog["indexes"]
            for item in json.loads((repo / index).read_bytes())["entries"]]
    return rows + [{"coverage": "all-entry", **catalog["fixture"]}]


def asset_root(repo, model):
    return model.parent if model.parent == repo / "examples/runtime/fixture" else repo


def compact(data):
    return json.dumps(data, separators=(",", ":"), ensure_ascii=False, allow_nan=False)


def write_authored(path, data):
    def records(rows, spaces):
        if not rows:
            return "[]"
        indent = " " * spaces
        return "[\n" + ",\n".join(indent + compact(row) for row in rows) + "\n" + indent[:-2] + "]"

    sections = []
    for key, value in data.items():
        if key == "components":
            contents = ",\n".join("    " + compact(part) + ": " + records(rows, 6)
                                  for part, rows in value.items())
            text = "{\n" + contents + "\n  }"
        elif key in ("port_flow_tables", "port_flow_scales"):
            text = records(value, 4)
        else:
            text = compact(value)
        sections.append("  " + compact(key) + ": " + text)
    path.write_text("{\n" + ",\n".join(sections) + "\n}\n", encoding="utf-8")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def execute(work, label, argv, expected=0):
    command = list(map(str, argv))
    result = subprocess.run(command, cwd=work, capture_output=True)
    (work / (label + ".log")).write_bytes(
        ("$ " + shlex.join(command) + "\n").encode() + result.stdout + result.stderr +
        f"\nexit {result.returncode}\n".encode())
    require(result.returncode == expected,
            f"{label} exited {result.returncode}. See {work / (label + '.log')}")
    return result.stdout + result.stderr


def first_difference(expected, actual, path=""):
    if type(expected) in (int, float) and type(actual) in (int, float):
        return None if bits(expected) == bits(actual) else f"{path}: {expected!r} != {actual!r}"
    if isinstance(expected, dict) and isinstance(actual, dict) and expected.keys() == actual.keys():
        return next((difference for key in expected
                     if (difference := first_difference(expected[key], actual[key], f"{path}/{key}"))), None)
    if isinstance(expected, list) and isinstance(actual, list) and len(expected) == len(actual):
        return next((difference for index, (left, right) in enumerate(zip(expected, actual))
                     if (difference := first_difference(left, right, f"{path}/{index}"))), None)
    return None if type(expected) is type(actual) and expected == actual else f"{path}: {expected!r} != {actual!r}"


def author_document(source, imports, v1, origins):
    document = json.loads(v1)
    document["format_version"] = 2
    table = document["components"]["functions"]
    functions = {curve["id"]: {**curve, "kind": "samples"} for curve in table}
    harmonics = []
    for origin in origins:
        identifier = origin["id"]
        lift_key, lift = authored_lift(origin["max_lift_m"], source, imports)
        curve = {"id": identifier, "kind": "harmonic_cam_lobe",
                 "duration_at_50_thou_deg": author_number(origin["duration_at_50_thou_rad"], DEGREE),
                 lift_key: lift,
                 "gamma": origin["gamma"], "steps": origin["steps"]}
        functions[identifier] = curve
        harmonics.append(curve)
    texts = sources(source, imports)
    provenance = {key: str(path) for key, path in port_tables(texts, document, functions).items()}
    roles = {}
    parts = document["components"]
    for definition in parts["ignitions"]:
        roles[definition["timing_curve"]] = "ignition_timing"
    for definition in parts["camshafts"]:
        roles[definition["lobe_profile"]] = "cam_samples"
    for definition in parts["fuels"]:
        roles[definition["turbulence_to_flame_speed_ratio"]] = "fuel_flame_speed"
    default = document["engine"]["mean_piston_speed_to_turbulence"]
    functions[default] = {"id": default, "kind": "legacy_default_turbulence"}
    provenance[default] = "compiled legacy engine default"
    for lobe in harmonics:
        provenance[lobe["id"]] = "converter evaluated harmonic inputs"

    for original in table:
        key = original["id"]
        if functions[key]["kind"] != "samples":
            continue
        kind = roles.get(key)
        if kind in ("ignition_timing", "cam_samples"):
            migrated, path = source_points(texts, original, kind)
        elif kind == "fuel_flame_speed":
            migrated, path = fuel_curve(texts, original)
        else:
            raise ValueError(f"{key} has no authored curve mapping")
        functions[key] = migrated
        provenance[key] = str(path)
    document["components"]["functions"] = [functions[curve["id"]] for curve in table]
    return document, harmonics, provenance


def verify_model(args, item, candidate, work):
    reference = args.repo / item["model"]
    authored = candidate.read_bytes()
    expanded = work / "expanded.json"
    roundtrip = work / "roundtrip.json"
    execute(work, "expand", [args.writer, "--expand", candidate, expanded])
    actual = expanded.read_bytes()
    frozen = args.reference_dir / item["model"] if args.reference_dir else expanded
    expected = frozen.read_bytes()
    if actual != expected:
        difference = first_difference(json.loads(expected), json.loads(actual))
        raise ValueError(f"Expanded v1 bytes differ. First field difference: {difference}")
    if "v1_sha256" in item:
        require(sha(actual) == item["v1_sha256"], "Expanded model differs from the pinned v1 hash")
    require(all(curve["kind"] != "samples" for curve in json.loads(authored)["components"]["functions"]),
            "Legacy curve mapping is incomplete")
    execute(work, "authoring-roundtrip", [args.writer, candidate, roundtrip])
    require(roundtrip.read_bytes() == authored, "Writer did not preserve the authored JSON bytes")

    root = asset_root(args.repo, reference)
    cases = []
    for flavor, mode, rate, frames, controls in render_cases(item):
        key = f"{flavor}-{mode}-{rate}"
        schedule = work / (key + ".tsv")
        schedule.write_text(controls, encoding="ascii")
        baseline, migrated = (work / (key + suffix) for suffix in ("-v1.wav", "-v2.wav"))
        old = execute(work, key + "-v1", [args.renderer, *render_flags(frozen, root, mode, rate, frames, schedule, baseline)])
        new = execute(work, key + "-v2", [args.renderer, *render_flags(candidate, root, mode, rate, frames, schedule, migrated)])
        wave = baseline.read_bytes()
        require(len(wave) == 58 + 4 * frames and old == new and wave == migrated.read_bytes(),
                f"PCM or final RPM differs at {key}")
        samples = struct.unpack_from(f"<{frames}f", wave, 58)
        require(all(map(math.isfinite, samples)) and any(samples), f"Silent or nonfinite result at {key}")
        cases.append({"case": key, "frames": frames, "pcm_sha256": sha(wave[58:]), "result": new.decode().strip()})
    resources = {impulse["path"] for impulse in json.loads(expected)["components"]["impulses"]}
    return {"v1_bytes": len(expected), "v2_bytes": len(authored), "expanded_sha256": sha(actual), "renders": cases,
            "resources": {path: sha((root / path).read_bytes()) for path in sorted(resources)}}


def migrate(args, item, work):
    reference = args.repo / item["model"]
    source = args.repo / item["source"]
    selection = item["selection"]
    if selection not in ("main", "entry"):
        selection = "factory:" + selection
    reconverted = work / "reconverted.json"
    manifest = work / "converter.json"
    resources = asset_root(args.repo, reference)
    legacy_assets = args.repo / "assets" if resources == args.repo else resources
    library = args.repo / "es" if resources == args.repo else resources / "library"
    execute(work, "convert", [args.converter, "--entry", source, "--selector", selection,
                             "--library-root", library, "--asset-root", legacy_assets,
                             "--resource-root", resources, "--output", reconverted,
                             "--manifest", manifest])
    frozen = (args.reference_dir / item["model"]).read_bytes()
    source_model = reconverted.read_bytes()
    if source_model != frozen:
        difference = first_difference(json.loads(frozen), json.loads(source_model))
        raise ValueError(f"The .mr evaluation differs from frozen v1: {difference}")
    receipt = json.loads(manifest.read_bytes())
    imports = tuple(Path(row["canonical_source"]) for row in receipt["inputs"] if row["role"] == "import")
    candidate = work / reference.name
    document, harmonics, provenance = author_document(source, imports, frozen, receipt["harmonic_lobes"])
    write_authored(candidate, document)
    verified = verify_model(args, item, candidate, work)
    return {**verified, "candidate": str(candidate),  "harmonics": harmonics,
            "port_tables": document.get("port_flow_tables", []), "scales": document.get("port_flow_scales", []),
            "functions": [{"id": curve["id"], "kind": curve["kind"], "source": provenance[curve["id"]]}
                          for curve in document["components"]["functions"]]}


def visit(args, item, work):
    reference = args.repo / item["model"]
    report = {"source": item["source"], "model": item["model"], "v1_bytes": reference.stat().st_size}
    work.mkdir()
    try:
        if args.action == "migrate":
            report.update(migrate(args, item, work))
        else:
            if "v1_reason" in item:
                require(json.loads(reference.read_bytes())["format_version"] == 1, "Invalid v1 fallback")
                return {**report, "status": "v1", "reason": item["v1_reason"]}
            require("v1_sha256" in item, f"Missing v1 identity for {item['model']}")
            report.update(verify_model(args, item, reference, work))
    except (ValueError, OSError) as error:
        return {**report, "status": "v1", "reason": str(error), "evidence": str(work)}
    return {**report, "status": "verified", "evidence": str(work)}


def main(args, work):
    manifest = entries(args.repo)
    require(len(manifest) == 27, "Unexpected catalog denominator")
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        pending = [pool.submit(visit, args, item, work / f"{i:02d}-{Path(item['model']).stem}")
                   for i, item in enumerate(manifest)]
        rows = [run.result() for run in pending]
    (work / "summary.json").write_text(json.dumps(rows, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    for row in rows:
        print(row["status"], row["model"], row.get("reason", f"{len(row.get('renders', []))} passing renders"))
    if args.action == "verify":
        for item, row in zip(manifest, rows):
            expected = "v1" if "v1_reason" in item else "verified"
            require(row["status"] == expected and (expected == "verified" or bool(item.get("v1_reason"))),
                    f"Authoring regression in {item['model']}: {row.get('reason')}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("migrate", "verify"))
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--writer", required=True, type=Path)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--converter", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--jobs", type=int, default=4)
    options = parser.parse_args()
    if options.action == "migrate" and (not options.converter or not options.reference_dir or not options.out):
        parser.error("Migration requires the legacy converter, a frozen v1 reference directory and a fresh output directory")
    for name in ("repo", "writer", "renderer", "converter", "reference_dir"):
        if path := getattr(options, name):
            setattr(options, name, path.resolve(strict=True))
    if options.out:
        options.out = options.out.resolve()
        options.out.mkdir()
        main(options, options.out)
    else:
        with tempfile.TemporaryDirectory(prefix="engine-sim-authoring-") as temporary:
            main(options, Path(temporary))
