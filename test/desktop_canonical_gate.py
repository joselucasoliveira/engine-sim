"""Exercise the installed canonical desktop and its reload and PCM entry points."""
import argparse
import json
import os
import platform
import subprocess
import sys
import tempfile
from pathlib import Path


def read(path):
    return json.loads(path.read_bytes())


def need(condition, message):
    if not condition:
        raise ValueError(message)


class Gate:
    def __init__(self, args, work):
        self.args = args
        self.work = work
        work.mkdir(parents=True, exist_ok=True)
        self.commands = []
        self.env = dict(os.environ, ENGINE_SIM_TEST_SOURCE=str(args.source))

    def run(self, argv, *, failure=None, environment=None, timeout=120):
        argv = list(map(str, argv))
        log = self.work / f"{len(self.commands) + 1:02}.log"
        result = subprocess.run(argv, cwd=self.work, env=environment or self.env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=timeout)
        log.write_bytes(result.stdout)
        self.commands.append(dict(argv=argv, cwd=str(self.work), exit=result.returncode, log=log.name,
                                  expected_error=failure))
        (self.work / "commands.json").write_text(json.dumps(self.commands, indent=2) + "\n")
        if failure:
            need(result.returncode != 0 and failure in result.stdout.decode(errors="replace"),
                 f"expected desktop failure missing, see {log}")
        elif result.returncode:
            print(result.stdout.decode(errors="replace"))
            result.check_returncode()

    def select(self):
        args, work = self.args, self.work
        prefix = work / "installed"
        refused = work / "refused"
        install = [args.cmake, "--install", args.build, "--config", args.config, "--component", "Runtime", "--prefix"]
        self.run([*install, refused], environment=dict(self.env, ENGINE_SIM_STAGE_MODE="redistributable"),
                 failure="unresolved impulse-recording clearance")
        need(not any(path.is_file() for path in refused.rglob("*")), "rejected installation copied files")
        self.run([*install, prefix], environment=dict(self.env, ENGINE_SIM_STAGE_MODE="local-validation"))
        current = platform.system()
        if current == "Darwin":
            contents = prefix / "EngineSim.app/Contents"
            app, resources = contents / "MacOS/EngineSim", contents / "Resources"
        else:
            app = prefix / "bin" / ("engine-sim-app.exe" if current == "Windows" else "engine-sim-app")
            resources = prefix / "share/engine-sim"
        need(app.is_file(), "the installed desktop executable is missing")
        need((resources / "LOCAL_VALIDATION_ONLY.txt").is_file(), "local resource stage lacks its rights notice")
        need(not any(path.suffix.lower() == ".mr" for path in resources.rglob("*")),
             "desktop installation includes a script")

        start = resources / "assets/main.json"
        alternate = resources / "assets/engines/atg-video-1/05_honda_vtec.json"
        self.run([app, "--script", work / "unavailable.mr"], failure="engine-sim-convert")
        self.run([app, "--help"])
        smoke = args.source / "tools/run_desktop_smoke.py"
        cases = [("default", start), ("alternate", alternate)]
        settings = sorted((resources / "assets/themes").glob("*.json"))
        need(len(settings) == 6, "desktop stage must supply six theme objects")
        for theme in settings:
            model = work / f"theme {theme.stem}.json"
            payload = read(start)
            payload["desktop"] = read(theme)
            model.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
            cases.append((theme.stem, model))
        for index, (name, model) in enumerate(cases):
            result = work / f"application-{index}-{name}"
            command = [sys.executable, "-B", smoke, "--app", app, "--out", result,
                       "--audio-mode", "virtual", "--frames", "60", "--timeout", "100"]
            if index != 0:
                command += ["--resources", resources, "--model", model]
            self.run(command)
            report = read(result / "application.json")
            expected = read(model)
            need(report["desktop"] == expected["desktop"], f"unexpected effective settings for {name}")
            need(report["engine_name"] == expected["engine"]["name"], f"unexpected selected engine for {name}")
            need(report["resized"] and report["throttle_changed"], f"desktop controls did not progress for {name}")
            need(report["pacer_source_faults"] == 0 and report["prepared_pcm_frames"] >= report["pacer_consumed_source"] > 0,
                 f"unexpected PCM accounting for {name}")

    def reload(self):
        args, work = self.args, self.work
        resources = work / "resources"
        self.run([sys.executable, "-B", args.source / "tools/stage_resources.py", "--desktop", "--local-validation",
                  "--source", args.source, "--manifest", args.source / "cmake/runtime-assets.json",
                  "--platform", platform.system(), "--destination", resources])
        need(not any(path.suffix.lower() == ".mr" for path in resources.rglob("*")),
             "reload test resource root contains scripts")
        self.env["ENGINE_SIM_TEST_RESOURCES"] = str(resources)
        self.env["ENGINE_SIM_DESKTOP_PCM_EVIDENCE"] = str(work)
        self.run([args.pacing, "--gtest_filter=RuntimeDesktop.*:DesktopPacing.PauseAndBoundedManualStepsDoNotReplaySource"])
        self.run([args.host, "--gtest_filter=HostApplication.FailedReturnReloadKeepsSessionAndSuccessfulReturnReplacesIt:"
                  "ViewsApplication.FailedDetachedViewsKeepPaletteGenerationControlsAndDrawableGeometry"])
        if args.audio:
            self.run([args.audio, "--gtest_filter=DesktopAudio.ActualNineCylinderSourcePreservesOrderAcrossRejectedAndAcceptedReload"])
        prepared, submitted, consumed = ((work / f"{name}.le16").read_bytes()
                                         for name in ("prepared", "submitted", "consumed"))
        need(bool(consumed) and prepared.startswith(submitted) and submitted.startswith(consumed),
             "the partial-write consumer lost or reordered a PCM prefix")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gate", choices=("select", "reload"))
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--cmake")
    parser.add_argument("--build", type=Path)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--pacing", type=Path)
    parser.add_argument("--host", type=Path)
    parser.add_argument("--audio", type=Path)
    args = parser.parse_args()
    if output := os.environ.get("ENGINE_SIM_GATE_OUTPUT"):
        work = Path(output).resolve(strict=True) / ("desktop-" + args.gate)
        need(not work.exists(), "desktop evidence already exists")
        getattr(Gate(args, work), args.gate)()
    else:
        with tempfile.TemporaryDirectory(prefix="engine-sim-desktop-") as directory:
            getattr(Gate(args, Path(directory)), args.gate)()
