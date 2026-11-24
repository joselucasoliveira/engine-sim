"""Verify lawful fixture identities and the two standalone host behaviors."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verify(reverse, first, second):
    assert digest(first.read_bytes()) == "e443a08b0c57d24792905423bcae9f5d6d0a25a299d3b438a9350932ef6fbdf9"
    assert digest(second.read_bytes()) == "91ec13e56b6e759f8d21d4153a2a810ce62a9816393af14c8509ca80bfac7a45"
    impulse = "97fb8b13c2ed77769f297659db76203f86d90d56201e02df38b5a295a1bb336b"
    for file in (first, second):
        assert digest((file.parent / "impulse.wav").read_bytes()) == impulse
    if reverse:
        with tempfile.TemporaryDirectory(prefix="fleet-model-") as folder:
            expanded = Path(folder) / "expanded.json"
            subprocess.run([reverse, "--expand", str(second), str(expanded)], check=True)
            data = (json.dumps(json.loads(expanded.read_text(encoding="utf-8")),
                               sort_keys=True, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
            expected = "0dc441e0f9a8d2b740e7fe44d8a2371023f44184a96f972c57dbf49ea531507f"
            assert digest(data) == expected
            print(f"canonical inertia derivative sha256={expected}", flush=True)


def run(host, first, second):
    env = os.environ.copy()
    for variable in ("DISPLAY", "WAYLAND_DISPLAY", "PULSE_SERVER", "PIPEWIRE_REMOTE"):
        env.pop(variable, None)
    print(f"device-neutral host={host} sha256={digest(Path(host).read_bytes())}", flush=True)
    args = [host, str(first), str(first.parent), str(second), str(second.parent)]
    process = subprocess.run(args, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             timeout=60, check=False)
    print(process.stdout, end="", flush=True)
    assert process.returncode == 0, f"{host} exited {process.returncode}"
    samples = re.findall(r"^lane=(\d) ticks=(\d+) nonzero=(\d+)$", process.stdout, re.M)
    assert [lane for lane, _, _ in samples] == ["0", "1"] and all(int(t) and int(n) for _, t, n in samples)
    assert re.search(r"^submitted=68000 generated=\d+ blocks=266 last=160$", process.stdout, re.M)
    for lane, start, end in ((0, 60000, 60240), (1, 66000, 66240)):
        assert f"stop lane={lane} start={start} end={end}" in process.stdout
        assert f"returned lane={lane} frames={end} ticks={end * 10000 // 48000}" in process.stdout
    return [(lane, nonzero) for lane, _, nonzero in samples]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("c_host")
    parser.add_argument("cxx_host")
    parser.add_argument("first", type=Path)
    parser.add_argument("second", type=Path)
    parser.add_argument("--expand")
    args = parser.parse_args()
    first, second = args.first.resolve(), args.second.resolve()
    verify(args.expand, first, second)
    assert run(args.c_host, first, second) == run(args.cxx_host, first, second), "host comparison"
