"""Exercise the exact embedded bytes and the configure-time schema guard."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


driver, compiler, schema = map(Path, sys.argv[1:])
data = schema.read_bytes()
digest = hashlib.sha256(data).hexdigest()
embedded = subprocess.check_output([driver, "embedding", schema], text=True).strip()
if digest != embedded:
    raise RuntimeError("The embedded schema identity differs from the file hash")

mutations = {
    "unsupported": ({"pattern": "anything"}, "Unsupported schema keyword"),
    "remote": ({"$ref": "https://example.invalid/schema"}, "Only local $defs refs are allowed"),
    "unresolved": ({"$ref": "#/$defs/unresolved"}, "Unresolved schema ref"),
    "cycle": ({"$ref": "#/$defs/unused"}, "Schema expansion cycle"),
    "subform": ({"type": ["integer", "string"]}, "Unsupported type form"),
}
with tempfile.TemporaryDirectory(prefix="engine-sim-schema-check-") as tree:
    root = Path(tree)

    def reject(path, expected_digest, reason):
        target = root / "rejected.hpp"
        result = subprocess.run([sys.executable, "-B", compiler, path, target, "--expected-sha256", expected_digest],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode == 0 or reason not in result.stdout or target.exists():
            raise RuntimeError(f"Guard failed to reject {path.name}: {result.stdout}")
        print(reason)

    for name, (rule, reason) in mutations.items():
        changed = json.loads(data)
        changed["$defs"]["unused"] = rule
        path = root / (name + ".json")
        path.write_text(json.dumps(changed), encoding="utf-8")
        reject(path, hashlib.sha256(path.read_bytes()).hexdigest(), reason)
    reject(path, digest, "Schema SHA-256 differs from the pinned registry entry")

print(digest)
