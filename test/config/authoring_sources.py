"""Recover the catalog's authored curve inputs from its selected legacy sources."""
import math
import re
import struct


DEGREE = 3.14159265359 / 180.0
RPM = 0.104719755
THOU = (1.0 / 100.0 * 2.54) / 1000.0
LENGTH = {"thou": THOU, "mm": 0.001, "m": 1.0}
NUMBER = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)"
POINT = r"\.add_sample\(\s*([^,()]+),\s*([^,()]+)\)"
POINT_BLOCK = re.compile(
    rf"\bfunction\s*(?:(?P<name>\w+)\s*)?\((?P<radius>[^()]+)\)\s*"
    rf"(?:(?P=name)\s*)?(?P<points>(?:{POINT}\s*)+)")
FLOW_POINT = r"\.add_flow_sample\(\s*([^,()]+),\s*([^,()]+)\)"
FLOW_BLOCK = re.compile(
    rf"\bfunction\s+(intake_flow|exhaust_flow)\(([^()]+)\)\s*\1\s*((?:{FLOW_POINT}\s*)+)")
NODE = re.compile(r"\b(?:public|private)\s+node\s+(\w+)[^\n{]*\{")
FLOW_FIXTURE = re.compile(rf"\.add_sample\(\s*({NUMBER}),\s*(?:k_28inH2O\(({NUMBER})\)|({NUMBER}))\s*\)")


def bits(value):
    return struct.pack("<d", value)


def sources(primary, imports):
    return {source: re.sub(r"//[^\n]*", "", source.read_text(encoding="utf-8"))
            for source in dict.fromkeys((primary, *imports))}


def literal(expression):
    return float(expression) if re.fullmatch(NUMBER, expression.strip()) else None


def nominal(expression, unit):
    expression = expression.strip()
    match = re.fullmatch(rf"({NUMBER})\s*\*\s*\w+\.{unit}", expression)
    if match:
        return float(match[1])
    if literal(expression) == 0:
        return float(expression)  # Preserve the sign of an authored zero.
    return None


def same(profile, radius, samples):
    return (bits(profile["filter_radius"]) == bits(radius) and len(profile["samples"]) == len(samples)
            and all(bits(point["x"]) == bits(x) and bits(point["y"]) == bits(y)
                    for point, (x, y) in zip(profile["samples"], samples)))


def source_points(texts, profile, kind):
    ignition = kind == "ignition_timing"
    unit = "rpm" if ignition else "deg"
    x_key, y_key = ("rpm", "advance_deg") if ignition else ("angle_deg", "lift_m")
    x_unit, y_unit = (RPM, DEGREE) if ignition else (DEGREE, 1.0)
    for source, text in texts.items():
        for block in POINT_BLOCK.finditer(text):
            radius = nominal(block["radius"], unit)
            if radius is None:
                continue
            points = [(nominal(x, unit), nominal(y, "deg") if ignition else literal(y))
                      for x, y in re.findall(POINT, block["points"])]
            if all(x is not None and y is not None for x, y in points) and same(
                    profile, radius * x_unit, [(x * x_unit, y * y_unit) for x, y in points]):
                return {"id": profile["id"], "kind": kind, f"filter_radius_{unit}": radius,
                        "points": [{x_key: x, y_key: y} for x, y in points]}, source
    raise ValueError(f"{profile['id']} {kind} has no matching authored source points")


def fuel_curve(texts, profile):
    for source, text in texts.items():
        for block in POINT_BLOCK.finditer(text):
            radius = literal(block["radius"])
            if radius is None:
                continue
            rows = [(literal(x), [literal(factor) for factor in y.split("*")])
                    for x, y in re.findall(POINT, block["points"])]
            if any(x is None or None in factors or len(factors) > 3 for x, factors in rows):
                continue
            points, output = [], []
            for x, parts in rows:
                point = {"turbulence_speed": x}
                if len(parts) >= 2 and bits(parts[-1]) == bits(x):
                    scale = parts[0] if len(parts) == 3 else 1.0
                    multiplier = parts[-2]
                    y = (scale * multiplier) * x
                    point["flame_speed_multiplier"] = multiplier
                elif len(parts) <= 2:
                    scale = parts[0] if len(parts) == 2 else 1.0
                    y = scale * parts[-1]
                    point["flame_speed"] = parts[-1]
                else:
                    break
                if len(parts) == 3 or (len(parts) == 2 and "flame_speed" in point):
                    point["scale"] = scale
                output.append((x, y))
                points.append(point)
            if len(points) == len(rows) and same(profile, radius, output):
                return {"id": profile["id"], "kind": "fuel_flame_speed",
                        "filter_radius": radius, "points": points}, source
    raise ValueError(f"{profile['id']} has no matching authored flame-speed recipe")


def port_coefficient(scfm):
    # Candidate selection. The compiled expansion and frozen v1 comparator
    # remain the acceptance authority.
    atm, drop, hcr = 101.325 * 1000, 28.0 * (3386.3886666666713 * 0.0734824), 1.0 + (2.0 / 5)
    pressure_ratio = (atm - drop) / atm
    denominator = (2.0 * hcr) / (hcr - 1)
    denominator *= 1 - math.pow(pressure_ratio, (hcr - 1) / hcr)
    denominator = math.sqrt(denominator)
    denominator *= math.pow(pressure_ratio, 1 / hcr)
    denominator *= atm / math.sqrt(8.31446261815324 * (25 + 273.15))
    return (scfm * (0.002641 * 453.59237 / 60.0)) / denominator


def factor(expression, name):
    match = re.fullmatch(rf"({NUMBER})\s*\*\s*{name}", expression.strip())
    return (float(match[1]), True) if match else (literal(expression), False)


def flow_tables(texts):
    calls = {name for text in texts.values() for name in re.findall(r"\b(\w+)\s*\(", text)}
    for source, text in texts.items():
        factories = list(NODE.finditer(text))
        for start, stop in zip(factories, [*factories[1:], None]):
            factory = start[1]
            if factory not in calls:
                continue
            body = text[start.end():stop.start() if stop else len(text)]
            found = {}
            for block in FLOW_BLOCK.finditer(body):
                side, radius, points = block.group(1, 2, 3)
                unit = next((unit for unit in LENGTH if nominal(radius, unit) is not None), None)
                if unit is None:
                    continue
                rows = [(factor(x, "lift_scale"), factor(y, "flow_attenuation"))
                        for x, y in re.findall(FLOW_POINT, points)]
                if any(x is None or y is None for (x, _), (y, _) in rows):
                    continue
                modes = {(lift_scaled, flow_scaled) for (_, lift_scaled), (_, flow_scaled) in rows}
                if len(modes) != 1:
                    continue
                table = {"id": f"{factory}-{side}", "lift_unit": unit,
                         "filter_radius": nominal(radius, unit),
                         "points": [dict(lift=x, flow_scfm=y) for (x, _), (y, _) in rows]}
                found[side] = (table, modes.pop())
            if found.keys() == {"intake_flow", "exhaust_flow"}:
                yield source, found


def fixture_port(texts, profile):
    for source, text in texts.items():
        function = re.search(rf"\bfunction\s+port\(({NUMBER})\)\s*port(?P<points>(?:{FLOW_FIXTURE.pattern}\s*)+)", text)
        if not function:
            continue
        rows = [(float(x), float(flow or zero)) for x, flow, zero in FLOW_FIXTURE.findall(function["points"])]
        radius = float(function[1])
        if same(profile, radius, [(lift, port_coefficient(flow)) for lift, flow in rows]):
            return source, {"id": "measured-port", "lift_unit": "m", "filter_radius": radius,
                            "points": [dict(lift=lift, flow_scfm=flow) for lift, flow in rows]}
    raise ValueError(f"{profile['id']} has no matching shared single-port source")


def port_tables(texts, document, functions):
    candidates = list(flow_tables(texts))
    scales = {
        field: {1.0, *(float(value) for text in texts.values()
                       for value in re.findall(rf"\b{field}\s*:\s*({NUMBER})\b", text))}
        for field in ("lift_scale", "flow_attenuation")
    }
    table_bank, groups = {}, {}
    origins = {}
    original = {curve["id"]: curve for curve in document["components"]["functions"]}
    radial_factory = any(re.search(r"\bradial_head\s*\(", text) for text in texts.values())

    def matches(profile, table, lift, flow):
        unit = LENGTH[table["lift_unit"]]
        return same(profile, table["filter_radius"] * unit,
                    [((point["lift"] * lift) * unit, port_coefficient(point["flow_scfm"] * flow))
                     for point in table["points"]])

    def add(key, table, source, scale_id=None):
        record = {"id": key, "kind": "port_flow_28_inh2o", "table": table["id"]}
        if scale_id is not None:
            record["scale"] = scale_id
        if key in origins and functions[key] != record:
            raise ValueError(f"{key} has conflicting source ownership")
        table_bank[table["id"]] = table
        functions[key] = record
        origins[key] = source

    for head in document["components"]["heads"]:
        intake, exhaust = (head[name + "_port_flow"] for name in ("intake", "exhaust"))
        if intake == exhaust:
            source, table = fixture_port(texts, original[intake])
            add(intake, table, source)
            continue
        admitted = None
        for source, tables in candidates:
            (a, a_flags), (b, b_flags) = (tables[side] for side in ("intake_flow", "exhaust_flow"))
            if a_flags != b_flags:
                continue
            lifts = sorted(scales["lift_scale"]) if a_flags[0] else (1.0,)
            flows = sorted(scales["flow_attenuation"]) if a_flags[1] else (1.0,)
            found = [(lift, flow) for lift in lifts for flow in flows
                     if matches(original[intake], a, lift, flow) and matches(original[exhaust], b, lift, flow)]
            if len(found) == 1:
                admitted = (source, a, b, a_flags, found[0])
                break
        if admitted is None:
            raise ValueError(f"{head['id']} has no bit-matched authored intake/exhaust source")
        source, a, b, flags, (lift, flow) = admitted
        scale_id = None
        if any(flags):
            scale_id = "radial-head-ports" if radial_factory else head["id"] + "-ports"
        if scale_id:
            group = {"id": scale_id}
            if lift != 1:
                group["lift_scale"] = lift
            if flow != 1:
                group["flow_attenuation"] = flow
            if groups.setdefault(scale_id, group) != group:
                raise ValueError(f"{scale_id} has inconsistent declared scale")
        add(intake, a, source, scale_id)
        add(exhaust, b, source, scale_id)

    if table_bank:
        document["port_flow_tables"] = list(table_bank.values())
    if groups:
        document["port_flow_scales"] = list(groups.values())
    return origins
