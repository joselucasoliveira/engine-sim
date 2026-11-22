"""Generate the fixture's original finite impulse response."""
import argparse
from pathlib import Path
import struct


def impulse_bytes():
    samples = (32767, 8192, -4096, 2048, -1024, 512, -256, 128)
    payload = struct.pack("<8h", *samples)
    return (b"RIFF" + struct.pack("<I", 36 + len(payload)) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, 1, 44100, 88200, 2, 16) +
            b"data" + struct.pack("<I", len(payload)) + payload)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_bytes(impulse_bytes())
