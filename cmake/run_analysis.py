"""Analyze selected ownership and buffer sources using their actual compilation commands."""

import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shlex
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--frontend", action="store_true")
    parser.add_argument("--runtime", action="store_true")
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--converter", action="store_true")
    parser.add_argument("--host-resources", action="store_true")
    parser.add_argument("--sdl-window", action="store_true")
    parser.add_argument("--sdl-renderer", action="store_true")
    parser.add_argument("--sdl-audio", action="store_true")
    parser.add_argument("--sdl-host", action="store_true")
    options = parser.parse_args()
    source = Path(__file__).resolve().parent.parent
    output = options.out.resolve()
    if not output.is_absolute() or output.is_relative_to(source):
        parser.error("analysis output must be outside the source checkout")
    output.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="analysis-", dir=output))
    selected = {source / name for name in ("src/audio_buffer.cpp", "src/convolution_filter.cpp",
                "src/synthesizer.cpp", "src/engine.cpp", "src/wave_reader.cpp")}
    if options.runtime:
        selected.update(source / name for name in ("src/runtime/api.cpp", "src/runtime/runtime.cpp",
                        "src/runtime/json_runtime.cpp", "src/simulator.cpp", "src/piston_engine_simulator.cpp",
                        "src/sample_clock.cpp", "src/pull_engine.cpp"))
    if options.capture:
        selected.update(source / name for name in ("src/runtime/runtime.cpp", "src/runtime/capture_runtime.cpp",
                        "src/runtime/json_runtime.cpp", "src/sample_clock.cpp", "src/pull_engine.cpp",
                        "src/simulator.cpp", "src/piston_engine_simulator.cpp", "tools/capture/worker.cpp"))
    if options.frontend:
        selected.update(source / name for name in ("src/desktop_math.cpp", "src/desktop_host.cpp", "src/desktop_run.cpp", "src/shaders.cpp",
                        "src/engine_sim_application.cpp", "src/geometry_generator.cpp", "src/oscilloscope.cpp",
                        "src/runtime/runtime.cpp", "src/runtime/json_runtime.cpp", "src/runtime/desktop_runtime.cpp",
                        "src/desktop_engine_views.cpp"))
    if options.converter:
        selected.update(source / name for name in ("scripting/src/compiler.cpp", "scripting/src/prepared_engine.cpp"))
    if options.host_resources:
        selected.update(source / name for name in ("src/resource_paths.cpp", "src/compiled_scene.cpp"))
    if options.sdl_window:
        selected.add(source / "src/sdl_window.cpp")
    if options.sdl_renderer:
        selected.update(source / name for name in ("src/sdl_renderer.cpp", "src/font_atlas.cpp"))
    if options.sdl_audio:
        selected.add(source / "src/sdl_audio.cpp")
    if options.sdl_host:
        selected.add(source / "src/sdl_host.cpp")
    baseline = json.loads((source / "cmake/analysis-baseline.json").read_text())
    records = []
    for entry in json.loads(options.database.read_text()):
        file = Path(entry["file"]).resolve()
        if file not in selected:
            continue
        original = entry.get("arguments") or shlex.split(entry["command"])
        args = [options.compiler]
        skip = False
        for arg in original[1:]:
            if skip:
                skip = False
                continue
            if arg in {"-o", "-MF", "-MT", "-MQ"}:
                skip = True
            elif arg not in {"-c", "-MD", "-MMD", "-MP"}:
                args.append(arg)
        report = run / (file.stem + ".plist")
        args += ["--analyze", "-Xanalyzer", "-analyzer-checker=core,cplusplus,unix",
                 "-Xanalyzer", "-analyzer-output=plist", "-o", str(report)]
        log = run / (file.stem + ".log")
        with log.open("w") as stream:
            result = subprocess.run(args, cwd=entry["directory"], stdout=stream, stderr=subprocess.STDOUT, timeout=120)
        report_data = plistlib.loads(report.read_bytes()) if report.exists() else None
        diagnostics = report_data["diagnostics"] if report_data is not None else None
        unexpected = []
        for diagnostic in diagnostics or []:
            location = diagnostic["location"]
            reported = Path(report_data["files"][location["file"]])
            if not reported.is_absolute():
                reported = Path(entry["directory"]) / reported
            reported = reported.resolve()
            lines = reported.read_text().splitlines()
            line = location["line"] - 1
            fingerprint = {"file": str(reported.relative_to(source)) if reported.is_relative_to(source) else str(reported), "check": diagnostic["check_name"],
                           "description": diagnostic["description"],
                           "source_line": lines[line].strip() if 0 <= line < len(lines) else "<invalid analyzer source location>"}
            if not any(all(fingerprint[key] == known[key] for key in fingerprint)
                       and ("source_sha256" not in known or hashlib.sha256(reported.read_bytes()).hexdigest() == known["source_sha256"])
                       for known in baseline):
                unexpected.append(diagnostic)
        records.append({"file": str(file.relative_to(source)), "argv": args, "cwd": entry["directory"],
                        "exit": result.returncode, "diagnostics": diagnostics, "unexpected": unexpected, "log": str(log)})
    (run / "result.json").write_text(json.dumps(records, indent=2) + "\n")
    print(f"analysis artifacts: {run}")
    if len(records) != len(selected) or any(row["exit"] != 0 or row["diagnostics"] is None
                                          or row["unexpected"] for row in records):
        print("analysis failed or selected ownership sources disappeared", file=sys.stderr)
        return 1
    print(f"analyzed {len(records)} ownership and buffer sources; no new diagnostics; "
          f"{sum(len(row['diagnostics']) for row in records)} reviewed legacy findings retained")
    return 0


if __name__ == "__main__":
    sys.exit(main())
