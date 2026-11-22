"""Measurements and comparisons for offline engine captures."""

import array
import csv
import functools
import hashlib
import math
from pathlib import Path
import statistics
import sys
import wave

RATE = 44100


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def raw_samples(path):
    if path.suffix not in {".f32le", ".s16le"}:
        raise ValueError(f"unknown sample format in {path}")
    floating = path.suffix == ".f32le"
    storage = array.array("f" if floating else "h")
    blob = path.read_bytes()
    if not blob or len(blob) % storage.itemsize:
        raise ValueError(f"missing or incomplete frames in {path}")
    storage.frombytes(blob)
    if sys.byteorder != "little":
        storage.byteswap()
    if floating and not all(math.isfinite(value) for value in storage):
        raise ValueError(f"nonfinite waveform in {path}")
    return storage, 1.0 if floating else 32768.0


@functools.lru_cache(maxsize=1)
def fft_parameters():
    size = 2048
    hann = tuple(0.5 * (1 - math.cos(2 * math.pi * n / (size - 1))) for n in range(size))
    reorder = tuple(int(f"{i:011b}"[::-1], 2) for i in range(size))
    roots = []
    for width in (2 ** exponent for exponent in range(1, 12)):
        roots.append(tuple(
            complex(math.cos(-2 * math.pi * j / width), math.sin(-2 * math.pi * j / width))
            for j in range(width // 2)
        ))
    return hann, reorder, roots


def spectrum(samples):
    hann, reorder, roots = fft_parameters()
    size = len(hann)
    normalization = size * sum(value * value for value in hann)
    bands = (20.0, 100.0, 500.0, 2000.0, 8000.0, RATE / 2)
    edges = [math.ceil(edge * size / RATE) for edge in bands]
    edges[-1] += 1
    window_energy = []
    integrated = [0.0] * 5
    for start in range(0, len(samples) - size + 1, 1024):
        block = [samples[start + i] * hann[i] for i in reorder]
        time_energy = sum(sample * sample for sample in block) / size
        window_energy.append(time_energy)
        if time_energy == 0.0:
            continue
        for stage in roots:
            half = len(stage)
            for offset in range(0, size, 2 * half):
                for j, root in enumerate(stage):
                    i = offset + j
                    item = root * block[i + half]
                    original = block[i]
                    block[i] = original + item
                    block[i + half] = original - item
        for band in range(5):
            integrated[band] += sum(
                (1 if j == size // 2 else 2) * abs(block[j]) ** 2 / normalization
                for j in range(edges[band], edges[band + 1])
            )
    return {
        "fft_length": size,
        "hop": 1024,
        "band_edges_hz": bands,
        "mean_band_power": [value / len(window_energy) for value in integrated] if window_energy else [],
        "window_energy": window_energy,
    }


def summarize_signal(path, checksum, cache):
    if checksum in cache:
        return cache[checksum]
    storage, scale = raw_samples(path)
    values = [sample / scale for sample in storage]
    active = [i for i, value in enumerate(storage) if value != 0]
    rail_count = longest_rail = run = 0
    if scale != 1.0:
        for sample in storage:
            if sample in (-32768, 32767):
                rail_count += 1
                run += 1
                longest_rail = max(longest_rail, run)
            else:
                run = 0
    differences = sorted(abs(a - b) for a, b in zip(values, values[1:]))
    features = {
        "sha256": checksum,
        "frames": len(storage),
        "sample_rate": RATE,
        "dc": statistics.fmean(values),
        "rms": math.sqrt(statistics.fmean(value * value for value in values)),
        "peak": max(map(abs, values)),
        "rail_count": rail_count,
        "longest_rail_run": longest_rail,
        "leading_zero_frames": active[0] if active else len(storage),
        "trailing_zero_frames": len(storage) - 1 - active[-1] if active else len(storage),
        "adjacent_change": quantiles(differences),
        "spectrum": spectrum(values),
    }
    cache[checksum] = features
    return features


def write_wave(path, target):
    storage, scale = raw_samples(path)
    if scale == 1.0:
        storage = array.array("h", (round(sample * 16384) for sample in storage))
    if sys.byteorder != "little":
        storage.byteswap()
    with wave.open(str(target), "wb") as stream:
        stream.setparams((1, 2, RATE, 0, "NONE", "not compressed"))
        stream.writeframes(storage.tobytes())


def quantiles(sorted_values, min_for_p99=1):
    if not sorted_values:
        return {}
    count = len(sorted_values)
    result = {
        "n": count,
        "p50": sorted_values[(count - 1) // 2],
        "p95": sorted_values[math.ceil(0.95 * count) - 1],
        "max": sorted_values[-1],
    }
    if count >= min_for_p99:
        result["p99"] = sorted_values[math.ceil(0.99 * count) - 1]
    return result


def summarize_trace(path):
    identity = hashlib.sha256()
    timing = {}
    bounds = {}
    rows = 0
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if not reader.fieldnames or len(reader.fieldnames) != len(set(reader.fieldnames)):
            raise ValueError(f"invalid trace header in {path}")
        physical = [name for name in reader.fieldnames if not name.endswith("_us")]
        identity.update((",".join(physical) + "\n").encode())
        previous_time = -math.inf
        for row in reader:
            if None in row or None in row.values():
                raise ValueError(f"truncated or invalid trace row in {path}")
            numbers = {name: float(value) for name, value in row.items()}
            if not all(math.isfinite(value) for value in numbers.values()):
                raise ValueError(f"nonfinite trace in {path}")
            if "sim_s" in numbers:
                if numbers["sim_s"] <= previous_time:
                    raise ValueError(f"non-monotonic simulated time in {path}")
                previous_time = numbers["sim_s"]
            identity.update((",".join(row[name] for name in physical) + "\n").encode())
            for name, value in numbers.items():
                if name.endswith("_us"):
                    if value < 0:
                        raise ValueError(f"negative duration in {path}")
                    timing.setdefault(name, []).append(value)
                else:
                    low, high = bounds.get(name, (value, value))
                    bounds[name] = [min(value, low), max(value, high)]
            rows += 1
    if not rows:
        raise ValueError(f"empty trace in {path}")
    return {
        "rows": rows, "physical_sha256": identity.hexdigest(), "bounds": bounds,
        "block_cost": {name: quantiles(sorted(values), 10000) for name, values in timing.items()},
    }


def phase_lag(reference, candidate):
    """Estimate the offset within ±220 frames, with time zero kept fixed for comparisons."""
    base, _ = raw_samples(Path(reference))
    trial, _ = raw_samples(Path(candidate))
    if not any(base) or not any(trial):
        return None
    length = min(len(base), len(trial))
    if length <= 440:
        return None
    offsets = range(-220, 221)
    step = max(1, (length - 440) // 4096)
    indices = range(220, length - 220, step)
    base_mean = statistics.fmean(base[i] for i in indices)
    centered = [(i, base[i] - base_mean) for i in indices]
    norm = math.sqrt(sum(value * value for _, value in centered))
    if norm == 0:
        return None

    def score(offset):
        trial_mean = statistics.fmean(trial[i + offset] for i, _ in centered)
        dot = energy = 0.0
        for i, value in centered:
            shift = trial[i + offset] - trial_mean
            dot += value * shift
            energy += shift * shift
        return dot / (norm * math.sqrt(energy)) if energy else -math.inf

    best = max(offsets, key=lambda offset: (score(offset), -abs(offset)))
    return best


def mechanical_rows(path):
    with path.open(newline="", encoding="utf-8") as stream:
        yield from csv.DictReader(stream)


def require_mechanics(condition, message):
    if not condition:
        raise ValueError(message)


def validate_mechanics(directory, name, frames):
    frequency, cylinders = (30000, 2) if name == "MECHANICS-KOHLER" else (10000, 8)
    ticks_per_block = frequency // 100
    ticks = 4 * frequency
    period = 4 * math.pi
    previous_phase = travel = max_step_error = 0.0
    powered_peak = coast_speed = final_speed = 0.0
    fixed_plate_min = math.inf
    fixed_plate_max = -math.inf
    tick = -1
    for tick, row in enumerate(mechanical_rows(directory / "mechanics-v1.csv")):
        require_mechanics(int(row["tick"]) == tick, "missing or duplicate mechanical tick")
        phase, speed, demand, plate = (float(row[key]) for key in
                                      ("cycle_rad", "crank_rad_s", "demand", "plate_rad"))
        require_mechanics(all(math.isfinite(value) for value in (phase, speed, demand, plate)),
                f"nonfinite mechanics at tick {tick}")
        block = (tick - 1) // ticks_per_block
        starter = 25 <= block < 125
        ignition = 25 <= block < 300
        expected_demand = 0.0 if tick == 0 or block >= 300 else 0.10 if block < 150 else 0.35
        require_mechanics(demand == expected_demand and int(row["starter"]) == starter
                and int(row["ignition"]) == ignition, f"control schedule differs at tick {tick}")
        require_mechanics(0 <= phase < period and 0 <= plate <= math.pi / 2, "angle outside its physical range")
        if tick == 0:
            require_mechanics(speed == 0.0, "prepared crank must be at rest")
        else:
            # Positive mechanical velocity is counterclockwise. Positive cycle
            # phase runs clockwise. Compare across wraps at the native tick.
            increment = math.remainder(phase - previous_phase, period)
            error = abs(increment + speed / frequency)
            require_mechanics(error <= 1e-10, f"phase and solved speed disagree at tick {tick}")
            max_step_error = max(max_step_error, error)
            travel += increment
        previous_phase = phase
        if 150 <= block < 300:
            powered_peak = max(powered_peak, -speed)
            fixed_plate_min = min(fixed_plate_min, plate)
            fixed_plate_max = max(fixed_plate_max, plate)
            if name == "MECHANICS-LS":
                expected_plate = (1 - 0.996 * (1 - demand**2)) * math.pi / 2
                require_mechanics(abs(plate - expected_plate) < 1e-12, "direct linkage disagrees with preset")
        if tick == 3 * frequency:
            coast_speed = -speed
        final_speed = -speed
    require_mechanics(tick == ticks and travel > period and powered_peak > 50,
            "trace lacks sustained forward motion after starter release")
    require_mechanics(coast_speed > 0 and final_speed < coast_speed, "ignition-off phase lacks coastdown")
    if name == "MECHANICS-KOHLER":
        require_mechanics(fixed_plate_max - fixed_plate_min > 1e-4,
                "governed plate failed to move under constant demand")

    previous_event = (-1, -1)
    lit = set()
    powered_count = 0
    for row in mechanical_rows(directory / "combustion-v1.csv"):
        event = tick_id, cylinder = int(row["tick"]), int(row["cylinder"])
        require_mechanics(event > previous_event and 25 * ticks_per_block < tick_id <= 3 * frequency
                and 0 <= cylinder < cylinders, "combustion event ordering or range differs")
        previous_event = event
        if tick_id > 125 * ticks_per_block:
            lit.add(cylinder)
            powered_count += 1
    require_mechanics(len(lit) == cylinders, "all cylinders must actually light after starter release")

    previous_frames = 0
    block = 0
    for block, row in enumerate(mechanical_rows(directory / "queues-v1.csv"), 1):
        total = int(row["delivered"])
        require_mechanics(int(row["tick"]) == block * ticks_per_block and abs(total - block * 441) <= 1,
                "legacy 10 ms grid and delivered sample count disagree")
        require_mechanics(total - previous_frames == int(row["block_frames"])
                and total == int(row["rendered"]), "rendered and delivered frame counts disagree")
        require_mechanics(int(row["pending_input"]) == int(row["block_frames"])
                and all(int(row[key]) == 0 for key in
                        ("queued_output", "dropped_input", "dropped_output", "zero_filled")),
                "capture lost, padded or retained frames")
        previous_frames = total
    require_mechanics(block == 400 and previous_frames == frames, "incomplete mechanical capture")
    return {"ticks": ticks, "frames": frames, "clockwise_travel_rad": travel,
            "max_tick_phase_error_rad": max_step_error, "powered_peak_rad_s": powered_peak,
            "post_starter_combustions": powered_count, "coast_start_rad_s": coast_speed,
            "coast_end_rad_s": final_speed, "plate_range_rad": [fixed_plate_min, fixed_plate_max]}
