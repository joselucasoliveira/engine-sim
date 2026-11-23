"""Render explicit canonical entry cases against pinned original R2 renderer outputs.

Each version-1 manifest contains an entries array. Each entry supplies key,
model, root and cases. Each case supplies mode, rate, seed, frames, schedule,
reference and sha256. Model, root and schedule paths resolve from the manifest.
Reference paths resolve beneath --reference-dir. Save every attempted result
in a fresh --out directory.
"""
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path


def require(condition, detail):
    if not condition:
        raise ValueError(detail)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def check_wave(data, rate, frames):
    require(len(data) == 58 + frames * 4, "Reference length disagrees with declared frames")
    require(data[:4] == b"RIFF" and data[8:20] == b"WAVEfmt \x12\x00\x00\x00"
            and data[38:46] == b"fact\x04\x00\x00\x00" and data[50:54] == b"data",
            "Expected an original renderer RIFF float32 file")
    require(struct.unpack_from("<HHIIHH", data, 20) == (3, 1, rate, rate * 4, 4, 32)
            and struct.unpack_from("<I", data, 46)[0] == frames
            and struct.unpack_from("<I", data, 54)[0] == frames * 4,
            "Reference metadata disagrees with the selected case")


def compare(args):
    renderer = args.renderer.resolve(strict=True)
    reference_dir = args.reference_dir.resolve(strict=True)
    out = args.out.resolve()
    out.mkdir()
    require(reference_dir.is_dir(), "Reference root must be a directory")
    records = []
    keys = set()
    seen = set()
    for manifest in args.manifests:
        manifest = manifest.resolve(strict=True)
        description = json.loads(manifest.read_bytes())
        require(description["version"] == 1 and description["entries"], "Expected nonempty version-1 entries")
        for entry in description["entries"]:
            key = entry["key"]
            require(isinstance(key, str) and key and key not in keys and entry["cases"], "Invalid or duplicate entry")
            keys.add(key)
            model = (manifest.parent / entry["model"]).resolve(strict=True)
            root = (manifest.parent / entry["root"]).resolve(strict=True)
            for case in entry["cases"]:
                mode, rate, seed, frames = (case[name] for name in ("mode", "rate", "seed", "frames"))
                require(mode in ("free", "driven") and type(rate) is int and rate in (44100, 48000)
                        and type(seed) is int and 0 <= seed < 2**64 and type(frames) is int and frames >= 0,
                        "Invalid render profile")
                schedule = (manifest.parent / case["schedule"]).resolve(strict=True)
                case_key = (key, mode, rate, seed, frames, schedule)
                require(case_key not in seen, "Duplicate render case")
                seen.add(case_key)
                original = (reference_dir / case["reference"]).resolve(strict=True)
                require(original.is_relative_to(reference_dir) and original.is_file(), "Reference escapes its root")
                expected = original.read_bytes()
                require(digest(expected) == case["sha256"], "Reference digest differs")
                check_wave(expected, rate, frames)
                target = out / f"{len(records):04d}.wav"
                command = list(map(str, [renderer, "--entry", model, "--asset-root", root, "--rate", rate,
                    "--mode", mode, "--seed", seed, "--frames", frames, "--schedule", schedule, "--out", target]))
                result = subprocess.run(command, cwd=out, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                actual = target.read_bytes() if target.is_file() else b""
                records.append(dict(key=key, argv=command, exit=result.returncode, reference=str(original),
                    original_sha256=case["sha256"], model_sha256=digest(model.read_bytes()),
                    schedule_sha256=digest(schedule.read_bytes()), rendered_sha256=digest(actual),
                    match=result.returncode == 0 and expected == actual, output=result.stdout.decode(errors="replace")))
                (out / "results.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
                require(records[-1]["match"], f"R2 parity failed for {key} {mode} {rate}")
    print(f"Matched {len(records)} cases for {len(keys)} entries")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--reference-dir", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("manifests", nargs="+", type=Path)
    compare(parser.parse_args())
