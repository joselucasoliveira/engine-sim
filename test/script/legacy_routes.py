"""Project the synthetic multi-bank script with two imported impulse origins."""

from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import wave


binary, fixture, library, build = map(Path, sys.argv[1:5])
mode = sys.argv[5]

with tempfile.TemporaryDirectory(prefix="legacy-routes-", dir=build) as directory:
    root = Path(directory)
    inputs = root / "entry"
    inputs.mkdir()

    def prepare_resource(side, frames, gain):
        path = root / side
        path.mkdir()
        (path / "audio.mr").write_text(
            'import "runtime_fixture.mr"\n\n'
            f'public node {side}_audio {{\n'
            '    alias output __out: result;\n'
            f'    impulse_response result(filename: "response.wav", volume: {gain})\n'
            '}\n', encoding="utf-8")
        wav = path / "response.wav"
        with wave.open(str(wav), "wb") as output:
            output.setparams((1, 2, 44100, 0, "NONE", "not compressed"))
            output.writeframes(struct.pack("<" + "h" * len(frames), *frames))
        return wav

    prepare_resource("left", [32767, 410, 0, 0], 0.01)
    right = prepare_resource("right", [32000, -676, 0, 0, 0], 0.02)
    source = fixture.read_text(encoding="utf-8")

    def run(case, script, resource_root=root):
        main = inputs / (case + ".mr")
        main.write_text(script, encoding="utf-8")
        subprocess.run([binary, case, main, library, root, resource_root], check=True)

    if mode == "selection":
        run("selection", source)
        oversized_name = 'name: "' + 'X' * 4097 + '"'
        run("unsupported", source.replace('name: "Runtime Single"', oversized_name))
        run("unselected", source.replace("set_engine(first_motor)\n", "").replace("set_engine(motor)\n", ""))
    elif mode == "routing":
        run("routing", source)
        run("outside", source, root / "left")
        right.write_bytes(b"corrupted data from the unselected project side")
        run("shared", source.replace("impulse_response: second_ir", "impulse_response: first_ir"))
        run("format", source)
    else:
        raise ValueError(mode)
