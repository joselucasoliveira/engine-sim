"""Exercise authoring defaults and bounded expansion, then reload the edited LS via the SDK."""
import argparse
from copy import deepcopy
import json
import math
from pathlib import Path
import tempfile

from authoring_catalog import compact, execute, first_difference, require, sha
from authoring_sources import DEGREE, LENGTH, THOU, bits, port_coefficient


def read(path):
    return json.loads(path.read_bytes())


def cam(duration, lift, gamma, steps):
    # Independent formula check. Keep the original center, then successive
    # positive/negative pairs, including all zero-lift padding samples.
    angle = duration * DEGREE / 4
    lift *= THOU
    frequency = math.acos(math.pow((2 * (50 * THOU)) / lift, 1 / gamma) - 1) / angle
    extent = 3.14159265359 / frequency
    radius = extent / (steps - 5.0)
    values = [{"x": 0.0, "y": lift}]
    for i in range(1, steps):
        x = i * radius
        y = 0.0 if x >= extent else lift * math.pow(.5 + .5 * math.cos(frequency * x), gamma)
        values.extend((dict(x=x, y=y), dict(x=-x, y=y)))
    return radius, values


def fixture(args, work):
    ls = args.repo / "assets/engines/atg-video-2/07_gm_ls.json"
    index_file = read(ls.with_name("presets-07-12.json"))
    frozen_id = next(row["v1_sha256"] for row in index_file["entries"] if (args.repo / row["model"]) == ls)
    frozen_path = work / "ls-v1.json"
    execute(work, "expand-234", [args.writer, "--expand", ls, frozen_path])
    require(sha(frozen_path.read_bytes()) == frozen_id, "Expansion disagrees with the pinned GM LS v1")
    original = read(ls)
    reference = read(frozen_path)
    curves = original["components"]["functions"]
    index = next(i for i, row in enumerate(curves)
                 if row["kind"] == "harmonic_cam_lobe" and row["duration_at_50_thou_deg"] == 234)
    lobe = curves[index]
    require((lobe["gamma"], lobe["max_lift_thou"], lobe["steps"]) == (1.1, 551, 256), "Wrong LS reference")
    original_radius, original_samples = cam(234, 551, 1.1, 256)
    previous = reference["components"]["functions"][index]
    require(bits(original_radius) == bits(previous["filter_radius"]) and
            first_difference(original_samples, previous["samples"]) is None, "LS formula baseline mismatch")

    edited = deepcopy(original)
    edited["components"]["functions"][index]["duration_at_50_thou_deg"] = 240
    path = work / "240.json"
    path.write_text(compact(edited) + "\n")
    expected_radius, expected_points = cam(240, 551, 1.1, 256)
    previous.update(filter_radius=expected_radius, samples=expected_points)
    oracle = work / "240-explicit-v1.json"
    oracle.write_text(compact(reference) + "\n")
    expanded = work / "240-expanded.json"
    execute(work, "expand-240", [args.writer, "--expand", path, expanded])
    difference = first_difference(reference, read(expanded))
    require(difference is None, f"Edited model differs from formula or changed other data: {difference}")
    require(len(expected_points) == 511 and bits(original_radius) != bits(expected_radius), "Edit did not update the profile")

    rewrite = work / "240-roundtrip.json"
    execute(work, "preserve-240", [args.writer, path, rewrite])
    require(rewrite.read_bytes() == path.read_bytes(), "Writer lost authored parameters")
    execute(work, "v1", [args.writer, frozen_path, work / "v1-roundtrip.json"])
    require((work / "v1-roundtrip.json").read_bytes() == frozen_path.read_bytes(), "V1 rewrite changed frozen bytes")

    def reject(name, change, field):
        candidate = deepcopy(edited)
        change(candidate["components"]["functions"][index], candidate)
        invalid = work / (name + ".json")
        invalid.write_text(compact(candidate))
        output = work / ("refused-" + name + ".json")
        diagnostic = execute(work, name, [args.writer, "--expand", invalid, output], expected=1)
        require(f"/components/functions/{index}/{field}".encode() in diagnostic and not output.exists(),
                f"Incorrect diagnostic for {name}: {diagnostic!r}")
        return invalid

    reject("bad-steps", lambda row, _: row.update(steps=5), "steps")
    reject("fractional-steps", lambda row, _: row.update(steps=6.5), "steps")
    reject("too-many-steps", lambda row, _: row.update(steps=524289), "steps")
    reject("lift-at-threshold", lambda row, _: row.update(max_lift_thou=50), "max_lift_thou")
    reject("ambiguous-lift-unit", lambda row, _: row.update(max_lift_mm=14), "max_lift_mm")
    reject("duration-underflow", lambda row, _: row.update(duration_at_50_thou_deg=1e-320), "duration_at_50_thou_deg")
    invalid = reject("bad-gamma", lambda row, _: row.update(gamma=0), "gamma")
    reject("combined-sample-budget", lambda row, _: row.update(steps=524288), "steps")

    defaults = deepcopy(edited)
    derived = defaults["components"]["functions"][index]
    for key in ("gamma", "max_lift_thou", "steps"):
        del derived[key]
    default_path = work / "defaults.json"
    default_path.write_text(compact(defaults))
    default_result = work / "defaults-expanded.json"
    execute(work, "defaults", [args.writer, "--expand", default_path, default_result])
    radius, points = cam(240, 300, 1, 100)
    effective = read(default_result)["components"]["functions"][index]
    require(bits(effective["filter_radius"]) == bits(radius) and first_difference(effective["samples"], points) is None,
            "Omitted harmonic defaults changed")
    explicit = deepcopy(original)
    ignition = next(i for i, row in enumerate(curves) if row["kind"] == "ignition_timing")
    original_v1 = read(frozen_path)
    explicit["components"]["functions"][ignition] = {**original_v1["components"]["functions"][ignition],
                                                     "kind": "samples"}
    explicit_input = work / "explicit-profile.json"
    explicit_input.write_text(compact(explicit))
    explicit_output = work / "explicit-expanded.json"
    execute(work, "explicit-profile", [args.writer, "--expand", explicit_input, explicit_output])
    require(explicit_output.read_bytes() == frozen_path.read_bytes(), "A v1-style explicit profile changed")

    invalid_fuel = deepcopy(original)
    fuel = next(i for i, row in enumerate(curves) if row["kind"] == "fuel_flame_speed")
    invalid_fuel["components"]["functions"][fuel]["points"][0]["flame_speed_multiplier"] = 2
    invalid_fuel_input = work / "invalid-fuel.json"
    invalid_fuel_input.write_text(compact(invalid_fuel))
    diagnostic = execute(work, "invalid-fuel", [args.writer, "--expand", invalid_fuel_input,
                                               work / "refused-fuel.json"], expected=1)
    require(f"/components/functions/{fuel}/points/0/flame_speed_multiplier".encode() in diagnostic,
            "Fuel diagnostic missed the authored point")
    port_edit = deepcopy(original)
    table_map = {table["id"]: table for table in port_edit["port_flow_tables"]}
    first_head = port_edit["port_flow_scales"][0]
    first_head["flow_attenuation"] = .75
    port_input = work / "shared-port-scale.json"
    port_input.write_text(compact(port_edit))
    port_output = work / "shared-port-scale-expanded.json"
    execute(work, "shared-port-scale", [args.writer, "--expand", port_input, port_output])
    effective = read(port_output)["components"]["functions"]
    old_ports = read(frozen_path)["components"]["functions"]
    affected = 0
    for i, row in enumerate(curves):
        if row.get("scale") == first_head["id"]:
            table = table_map[row["table"]]
            oracle_points = [dict(x=point["lift"] * LENGTH[table["lift_unit"]], y=port_coefficient(point["flow_scfm"] * .75))
                             for point in table["points"]]
            require(first_difference(oracle_points, effective[i]["samples"]) is None,
                    f"Shared port scale did not apply to {row['id']}")
            affected += 1
        else:
            require(first_difference(old_ports[i], effective[i]) is None, "Port edit changed another profile")
    require(affected == 2, "The head scale must affect both of its flow curves")

    first_head["flow_attenuation"] = 1.7e308
    overflow = work / "invalid-port-scale.json"
    overflow.write_text(compact(port_edit))
    diagnostic = execute(work, "invalid-port-scale", [args.writer, "--expand", overflow, work / "refused-port.json"], expected=1)
    require(b"/port_flow_scales/0/flow_attenuation" in diagnostic, "Port error missed the authored scale")
    if args.reload:
        print(execute(work, "reload", [args.reload, args.repo, ls, path, oracle, invalid]).decode().strip())
    print(f"LS intake 234° → 240°, 511 points, filter radius {original_radius:.17g} → {expected_radius:.17g}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--writer", required=True, type=Path)
    parser.add_argument("--reload", type=Path)
    parser.add_argument("--out", type=Path)
    options = parser.parse_args()
    if options.out:
        options.out.mkdir()
        fixture(options, options.out)
    else:
        with tempfile.TemporaryDirectory(prefix="engine-sim-ls-authoring-") as directory:
            fixture(options, Path(directory))
