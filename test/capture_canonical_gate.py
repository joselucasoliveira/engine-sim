"""Run the capture worker and verifier with lawful, script-free data."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import tempfile


def read(path):
    return json.loads(path.read_bytes())


def save(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def need(value, message):
    if not value:
        raise ValueError(message)


class Gate:
    def __init__(self, args, work):
        self.args = args
        self.work = work
        self.cwd = work / "unrelated cwd"
        self.cwd.mkdir()
        self.command_log = []
        self.environment = {name: value for name, value in os.environ.items()
                            if name not in ("DISPLAY", "WAYLAND_DISPLAY", "PULSE_SERVER")}
        self.fixture = args.source / "examples/runtime/fixture"
        self.assets = work / "resources"
        self.assets.mkdir()
        self.binary = args.capture

    def run(self, argv, expected=0, marker=None):
        argv = list(map(str, argv))
        result = subprocess.run(argv, cwd=self.cwd, env=self.environment, capture_output=True, timeout=90)
        name = f"{len(self.command_log) + 1:02}.log"
        raw = result.stdout + result.stderr
        (self.work / name).write_bytes(raw)
        self.command_log.append({"argv": argv, "cwd": str(self.cwd), "exit": result.returncode, "log": name})
        save(self.work / "commands.json", self.command_log)
        need(result.returncode == expected and (marker is None or marker.encode() in raw),
             f"unexpected capture result, see {self.work / name}")
        return raw

    def copy_model(self, model, relative):
        destination = self.assets / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(model, destination)
        for impulse in read(model)["components"]["impulses"]:
            path = (self.assets / impulse["path"]).resolve()
            need(path.is_relative_to(self.assets), "lawful fixture locator escapes data root")
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.fixture / "impulse.wav", path)
        return destination

    def capture(self, name, suite, case, *, model=None):
        output = self.work / name
        command = [self.binary, "run", "--suite", suite, "--case", case, "--resources", self.assets,
                   "--out", output, "--seed", 209, "--repetitions", 1, "--mode", "sync"]
        if model is not None:
            command += ["--model", model]
        self.run(command)
        return output, read(output / "run.json")

    def replay(self):
        sys.path.insert(0, str(self.args.source / "tools/capture"))
        from capture_cli import observation, summarize_signal

        path = Path("assets/engines/atg-video-1/02_kohler_ch750.json")
        model = self.copy_model(self.args.source / path, path)
        case = "MECHANICS-KOHLER"
        before_dir, before = self.capture("reference", "mechanics", case)
        same_dir, same = self.capture("replay", "mechanics", case)
        old, new = (record["cases"][case][0]["main"] for record in (before, same))
        need(before["status"] == "pass" and same["status"] == "pass", "valid capture was not published")
        need(old["mechanics"] == new["mechanics"] and old["mechanics"]["ticks"] == 120000 and
             old["signal"]["frames"] == 176400, "unexpected native tick or sample extent")
        need(old["fixtures"]["resources/" + path.as_posix()] == sha(model), "selected model was not pinned")
        for name in ("audio.s16le", "mechanics-v1.csv", "combustion-v1.csv", "queues-v1.csv", "graph.tsv"):
            need(old["artifacts"][name] == new["artifacts"][name], f"replay changed {name}")
        need(old["trace"]["physical_sha256"] == new["trace"]["physical_sha256"],
             "replay changed a physical observation")
        policy = self.work / "policy.json"
        save(policy, {"version": 1, "baseline_sha256": sha(before_dir / "run.json"),
                      "tuple": before["build"]["tuple"], "seed": before["seed"], "mode": before["mode"],
                      "cases": {case: observation(before["cases"][case][0])}, "intentional_allowances": []})
        compare = [self.binary, "compare", "--reference", before_dir, "--policy", policy, "--candidate"]
        self.run([*compare, same_dir, "--out", self.work / "equal"])
        need(read(self.work / "equal/comparison.json")["status"] == "pass", "unchanged capture comparison failed")

        changed_dir = self.work / "altered"
        shutil.copytree(same_dir, changed_dir)
        changed = changed_dir / new["directory"]
        pcm = changed / "audio.s16le"
        signal = bytearray(pcm.read_bytes())
        frame = 167
        signal[frame * 2] ^= 1
        pcm.write_bytes(signal)
        new["artifacts"][pcm.name] = sha(pcm)
        new["signal"] = summarize_signal(pcm, new["artifacts"][pcm.name], {})
        need(struct.unpack_from("<h", signal, frame * 2) != struct.unpack_from(
            "<h", (before_dir / old["directory"] / pcm.name).read_bytes(), frame * 2), "PCM probe did not change a sample")
        shaft = changed / "mechanics-v1.csv"
        rows = shaft.read_text(encoding="utf-8").splitlines()
        tick = 231
        words = rows[tick + 1].split(",")
        need(int(words[0]) == tick, "shaft proof wrote the wrong tick")
        words[2] = "1"
        rows[tick + 1] = ",".join(words)
        shaft.write_text("\n".join(rows) + "\n", encoding="utf-8")
        new["artifacts"][shaft.name] = sha(shaft)
        save(changed_dir / "run.json", same)
        self.run([*compare, changed_dir, "--out", self.work / "different"], expected=1,
                 marker="candidate differs from the frozen comparison")
        difference = read(self.work / "different/comparison.json")
        need(difference["status"] == "fail" and
             {item["frame"] for item in difference["first_differences"] if "frame" in item} == {frame} and
             {(item["tick"], item["field"]) for item in difference["first_differences"] if "tick" in item}
             == {(tick, "crank_rad_s")}, "comparison failed to locate the altered frame and tick")

    def script_free(self):
        prefix = self.work / "standalone"
        bin_dir = prefix / "bin"
        bin_dir.mkdir(parents=True)
        self.binary = bin_dir / self.args.capture.name
        shutil.copy2(self.args.capture, self.binary)
        helpers = prefix / "share/engine-sim/tools/capture"
        helpers.mkdir(parents=True)
        for name in ("capture_cli.py", "capture_metrics.py", "build.txt"):
            shutil.copyfile(self.args.helpers / name, helpers / name)
        model = self.copy_model(self.fixture / "main.json", "assets/main.json")
        output, result = self.capture("load-default", "reference", "LOAD")
        cases = result["cases"]["LOAD"][0]
        need(result["status"] == "pass" and all(event["status"] == "pass" for event in cases.values()),
             "canonical load or a required error did not complete")
        valid = cases["valid"]
        need(valid["fixtures"]["resources/assets/main.json"] == sha(model) and
             valid["fixtures"]["resources/impulse.wav"] == sha(self.fixture / "impulse.wav"),
             "capture did not retain the exact JSON and response bytes")
        graph = (output / valid["directory"] / "graph.tsv").read_text(encoding="utf-8").split("\t")
        need(graph[0] == "Runtime Single" and graph[1] == "1", "unexpected lawful engine selected")

        explicit = self.work / "custom Ω.json"
        document = read(model)
        document["engine"]["name"] = "Canonical Capture Ω"
        save(explicit, document)
        output, result = self.capture("load-explicit", "reference", "LOAD", model=explicit)
        valid = result["cases"]["LOAD"][0]["valid"]
        selected = (output / valid["directory"] / "graph.tsv").read_text(encoding="utf-8").split("\t")
        need(selected[0] == document["engine"]["name"] and valid["fixtures"]["fixture/input.json"] == sha(explicit),
             "explicit external JSON was not selected")
        self.run([self.binary, "run", "--script", explicit], expected=2, marker="engine-sim-convert")
        need(not list(self.work.rglob("*.mr")), "the staged script-free run contains a script")
        if platform.system() == "Linux":
            need(self.args.nm is not None, "Linux capture gate requires nm")
            symbols = self.run([self.args.nm, "-C", self.binary]).decode(errors="replace")
            need(not any(name in symbols for name in ("piranha::", "es_script::Compiler", "legacy_runtime::")),
                 "the copied capture executable contains evaluator or compatibility symbols")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gate", choices=("replay", "script_free"))
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--capture", required=True, type=Path)
    parser.add_argument("--helpers", required=True, type=Path)
    parser.add_argument("--nm", type=Path)
    args = parser.parse_args()
    if root := os.environ.get("ENGINE_SIM_GATE_OUTPUT"):
        work = Path(root).resolve(strict=True) / ("capture-" + args.gate)
        work.mkdir()
        getattr(Gate(args, work), args.gate)()
    else:
        with tempfile.TemporaryDirectory(prefix="capture-canonical-") as name:
            getattr(Gate(args, Path(name)), args.gate)()


if __name__ == "__main__":
    main()
