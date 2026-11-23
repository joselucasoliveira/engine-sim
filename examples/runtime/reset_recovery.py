"""Exercise failed load, changed-resource reset and successful recovery using ABI v2."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import shutil
import tempfile

from ffi_smoke import ABI, Config, Controls, Diagnostic, Error, Telemetry, bind


ES_READY = 1
ES_RESOURCE = 6
ES_CONFIG = 12


def exercise(library, fixture):
    lib = bind(library)
    error = Error()
    config = Config()
    detail = Diagnostic()
    detail.size = C.sizeof(detail)
    detail.abi = ABI
    keep = C.c_void_p()
    control = C.c_void_p()

    def check(code):
        if code:
            raise RuntimeError(error.message.decode(errors="replace"))

    def timeline(handle):
        row = Telemetry()
        row.size = C.sizeof(row)
        row.abi = ABI
        check(lib.es_get_telemetry(handle, C.byref(row), C.byref(error)))
        return row.state, row.frames, row.native_frames, row.ticks

    def render(handle, count=257):
        pcm = (C.c_float * count)()
        written = C.c_uint32()
        check(lib.es_render(handle, len(pcm), pcm, len(pcm), C.byref(written), C.byref(error)))
        assert written.value == len(pcm)
        return bytes(pcm)

    check(lib.es_config_init(ABI, C.sizeof(config), C.byref(config), C.byref(error)))
    controls = Controls(C.sizeof(Controls), ABI, 1, 1, 0.25, 0.0, 0.0)
    try:
        with tempfile.TemporaryDirectory(prefix="engine-sim-reset-") as directory:
            root = Path(directory).resolve()
            for name in ("main.json", "impulse.wav"):
                shutil.copyfile(fixture / name, root / name)
            entry = root / "main.json"
            asset_root = str(root).encode("utf-8")
            for handle in (keep, control):
                check(lib.es_create(C.byref(config), C.byref(handle), C.byref(error)))
                check(lib.es_load_json_file(handle, str(entry).encode("utf-8"), asset_root,
                                            C.byref(detail), C.byref(error)))
                check(lib.es_set_controls(handle, C.byref(controls), C.byref(error)))
            first = render(keep, 44100)
            assert first == render(control, 44100) and any(first)
            bad = (C.c_uint8 * 2)(ord("{"), ord("}"))
            initial = timeline(keep)
            assert lib.es_load_json_bytes(keep, bad, len(bad), asset_root,
                                          C.byref(detail), C.byref(error)) == ES_CONFIG
            assert detail.code == b"CFG_SHAPE" and timeline(keep) == initial
            failures = {}
            for name in ("main.json", "impulse.wav"):
                target = root / name
                original = target.read_bytes()
                before = timeline(keep)
                target.write_bytes(original + b"\n")
                try:
                    status = lib.es_reset_detailed(keep, C.byref(detail), C.byref(error))
                    assert status == ES_RESOURCE
                    assert detail.code == b"CFG_RESOURCE_BYTES" and timeline(keep) == before
                    assert render(keep) == render(control)
                    failures[name] = detail.code.decode("ascii")
                finally:
                    target.write_bytes(original)
            for handle in (keep, control):
                check(lib.es_reset_detailed(handle, C.byref(detail), C.byref(error)))
                assert timeline(handle) == (ES_READY, 0, 0, 0)
                check(lib.es_set_controls(handle, C.byref(controls), C.byref(error)))
            pcm = render(keep, 44100)
            assert pcm == render(control, 44100) == first
            return {"failed_load": "CFG_SHAPE", "failed_resets": failures,
                    "reset_frames": 0, "first_pcm_sha256": hashlib.sha256(pcm).hexdigest()}
    finally:
        lib.es_destroy(keep)
        lib.es_destroy(control)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(exercise(args.library.resolve(), args.fixture.resolve()), indent=2))
