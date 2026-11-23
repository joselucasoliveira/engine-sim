"""Check canonical source-tree serialization in fresh processes."""

import hashlib
import json
import locale
import os
from pathlib import Path
import subprocess
import sys
import tempfile


executable, fixture = map(Path, sys.argv[1:])


def canonical_order(pairs):
    names = [name.encode("utf-8") for name, _ in pairs]
    if names != sorted(set(names)):
        raise RuntimeError("Canonical object keys are not in ascending UTF-8 byte order")
    return dict(pairs)


def non_c_locale():
    current = locale.setlocale(locale.LC_NUMERIC)
    try:
        for requested in ("de_DE.UTF-8", "pt_BR.UTF-8", "fr_FR.UTF-8", "German_Germany.1252"):
            try:
                actual = locale.setlocale(locale.LC_NUMERIC, requested)
            except locale.Error:
                continue
            if locale.localeconv()["decimal_point"] == ",":
                return actual
    finally:
        locale.setlocale(locale.LC_NUMERIC, current)
    return None


with tempfile.TemporaryDirectory(prefix="engine-sim-json-determinism-") as temporary:
    tree = Path(temporary)

    def emit(src, name, process_locale):
        out = tree / name
        subprocess.run([executable, src, out], check=True, env=dict(os.environ, LC_ALL=process_locale))
        return out

    first = emit(fixture, "first.json", "C")
    second = emit(first, "second.json", "C")
    raw = first.read_bytes()
    if second.read_bytes() != raw or raw.count(b"\n") != 1 or not raw.endswith(b"\n"):
        raise RuntimeError("Fresh-process output differs or the canonical LF is missing")
    model = json.loads(raw.decode("utf-8"), object_pairs_hook=canonical_order)
    if model["format_version"] != 1 or not set(("desktop", "initialization", "vehicle", "transmission")) <= model.keys():
        raise RuntimeError("The example did not write canonical v1 defaults")
    process_locale = non_c_locale()
    if process_locale:
        alternate = emit(fixture, "locale.json", process_locale)
        if alternate.read_bytes() != raw:
            raise RuntimeError(f"Serialization differs under {process_locale}")
        print("Same canonical bytes under C and", process_locale)
    else:
        print("No comma-decimal locale present, checked two C processes")
    print(hashlib.sha256(raw).hexdigest())
