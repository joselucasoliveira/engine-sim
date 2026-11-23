"""Run an installed desktop with a hard process timeout and retain independent evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--model", type=Path)
    parser.add_argument("--resources", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--audio-mode", choices=("device", "virtual"), required=True)
    parser.add_argument("--frames", type=int, default=240)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    source = Path(__file__).resolve().parent.parent
    if not all(path.is_absolute() for path in (args.app, args.model, args.resources, args.out) if path is not None):
        parser.error("app, model, resources and out must be absolute paths")
    app, output = args.app.resolve(), args.out.resolve()
    bundle = app.parent.name == "MacOS" and app.parent.parent.name == "Contents"
    installation = app.parents[3] if bundle else app.parent.parent
    default_root = app.parent.parent / "Resources" if bundle else installation / "share/engine-sim"
    resources = (args.resources or default_root).resolve()
    model = (args.model or resources / "assets/main.json").resolve()
    if not app.is_file() or not model.is_file() or not 60 <= args.frames <= 10000 or not 1 <= args.timeout <= 600:
        parser.error("require an app and canonical model, 60..10000 frames and a 1..600 second timeout")
    if output.is_relative_to(source) or output.is_relative_to(installation):
        parser.error("output must be outside source and installation directories")
    if output.exists():
        parser.error("output must be a new directory")
    output.mkdir(parents=True)
    working = output / "working"
    working.mkdir()
    report = output / "application.json"
    before = {"app": digest(app), "model": digest(model)}
    command = [str(app), "--smoke", "--frames", str(args.frames), "--audio-mode", args.audio_mode,
               "--report", str(report)]
    if args.resources:
        command += ["--resources", str(resources)]
    if args.model:
        command += ["--model", str(model)]
    start = time.monotonic()
    error, result = "", None
    with (output / "stdout.log").open("wb") as stdout, (output / "stderr.log").open("wb") as stderr:
        try:
            result = subprocess.run(command, cwd=working, stdout=stdout, stderr=stderr, timeout=args.timeout)
        except subprocess.TimeoutExpired:
            error = "process exceeded the hard timeout and was killed"
    evidence = {"argv": command, "cwd": str(working), "process_exit": result.returncode if result else 124,
                "timeout_seconds": args.timeout, "wall_seconds": time.monotonic() - start,
                "before_sha256": before, "error": error, "scope": "Installed application with scheduled native input. Host graphics and physical audio require separate inventory/loopback evidence."}
    try:
        if error or result is None or result.returncode:
            raise ValueError(error or "application returned nonzero")
        data = json.loads(report.read_text())
        if data["status"] != "pass" or data["audio_mode"] != args.audio_mode or data["presented_frames"] != args.frames:
            raise ValueError("application report does not match the requested successful run")
        required = ("simulation_steps", "model_packets", "generated_packets", "text_packets", "nonzero_source_frames",
                    "nonzero_frames_fed_to_backend", "pumped_source_frames", "SDL_or_virtual_input_consumed",
                    "prepared_pcm_frames", "pacer_consumed_source")
        if not all(data[key] > 0 for key in required) or not data["teardown_complete"] or (
            data["accepted_reloads"] != 1 or data["pacer_source_faults"]
        ):
            raise ValueError("required application stage did not advance")
        if args.audio_mode == "device" and data["audio_driver"] in ("dummy", "virtual-clocked-sink"):
            raise ValueError("a virtual endpoint cannot satisfy device mode")
        fnv = 14695981039346656037
        for value in model.read_bytes():
            fnv = ((fnv ^ value) * 1099511628211) & ((1 << 64) - 1)
        if Path(data["model"]).resolve() != model or int(data["model_fnv1a64"]) != fnv:
            raise ValueError("application model identity differs from the supplied bytes")
        images = []
        for path in sorted(output.glob("application-frame-*.ppm")):
            magic, dimensions, maximum, pixels = path.read_bytes().split(b"\n", 3)
            width, height = map(int, dimensions.split())
            if magic != b"P6" or maximum != b"255" or len(pixels) != width * height * 3:
                raise ValueError("malformed actual frame readback")
            background = pixels[:3]
            different = sum(pixels[i:i+3] != background for i in range(0, len(pixels), 3))
            if different < 1000:
                raise ValueError("readback contains insufficient visible content")
            images.append({"file": path.name, "width": width, "height": height,
                           "nonbackground_pixels": different, "sha256": digest(path)})
        if len(images) != 2:
            raise ValueError("missing first/final native readbacks")
        after = {"app": digest(app), "model": digest(model)}
        if after != before:
            raise ValueError("app or model bytes changed during execution")
        evidence.update(status="pass", after_sha256=after, images=images, application_sha256=digest(report))
    except (ValueError, KeyError, OSError) as failure:
        evidence.update(status="fail", error=str(failure))
    evidence["artifact_sha256"] = {str(path.relative_to(output)): digest(path) for path in sorted(output.rglob("*")) if path.is_file()}
    (output / "receipt.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(json.dumps({key: evidence[key] for key in ("status", "process_exit", "wall_seconds", "error")}))
    return 0 if evidence["status"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
