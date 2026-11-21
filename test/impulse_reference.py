"""Compare all script-declared impulses with independent PCM and convolution oracles."""

import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import wave


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def main():
    binary, resources = (Path(arg).resolve() for arg in sys.argv[1:])
    if Path.cwd().resolve().is_relative_to(resources):
        raise ValueError("independent decoder artifacts require an external working directory")
    library = resources / "es/sound-library/impulse_responses.mr"
    entries = re.findall(r'filename:\s*"([^"]+)"\s*,\s*volume:\s*([0-9.]+)', library.read_text())
    assert len(entries) == 10
    # Retain the exact fixtures and outputs even if a subprocess or comparison fails.
    output = Path(tempfile.mkdtemp(prefix="impulse-oracle-", dir=Path.cwd()))
    print(f"impulse oracle artifacts: {output}", flush=True)
    rows = []
    for index, (relative, volume) in enumerate(entries):
        path = library.parent / relative
        with wave.open(str(path), "rb") as decoder:
            assert (decoder.getnchannels(), decoder.getsampwidth(), decoder.getframerate(), decoder.getcomptype()) == (1, 2, 44100, "NONE")
            count = decoder.getnframes()
            pcm = decoder.readframes(count)
        prefix = output / str(index)
        command = [str(binary), "--decode", str(path), str(prefix), volume]
        result = subprocess.run(command, text=True, capture_output=True, timeout=60)
        (output / f"{index}-stdout.log").write_text(result.stdout)
        (output / f"{index}-stderr.log").write_text(result.stderr)
        assert result.returncode == 0, result.stderr
        assert int(result.stdout) == count
        assert prefix.with_suffix(".s16le").read_bytes() == pcm
        samples = struct.unpack(f"<{count}h", pcm)
        trimmed = min(10000, max((i + 1 for i, value in enumerate(samples) if abs(value) > 100), default=0))
        gain = f32(float(volume))
        expected = [f32(f32(gain * value) / 32767) for value in samples[:trimmed]]
        expected.extend([0.0] * (10008 - len(expected)))
        response = prefix.with_suffix(".f32le").read_bytes()
        assert response == struct.pack("<10008f", *expected), relative
        rows.append({"path": relative, "frames": count, "volume": volume,
                     "file_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                     "pcm_sha256": hashlib.sha256(pcm).hexdigest(),
                     "response_sha256": hashlib.sha256(response).hexdigest(), "argv": command})
    (output / "result.json").write_text(json.dumps(rows, indent=2) + "\n")
    print(f"matched {len(rows)} declared impulses, PCM bytes, frame counts, gain and controlled convolution")


if __name__ == "__main__":
    main()
