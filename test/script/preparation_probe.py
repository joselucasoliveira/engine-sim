"""Exercise preparation in subprocesses and require silent recovery."""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import wave


def main():
    binary, source, external = map(Path, sys.argv[1:])
    root = Path(tempfile.mkdtemp(prefix="parser-preparation-", dir=external))
    print(f"parser fixture evidence: {root}", flush=True)
    script_dir = root / "entry Ω"
    assets = root / "assets Ω"
    library = root / "library Ω"
    cwd = root / "unrelated cwd"
    for path in (script_dir, assets, cwd):
        path.mkdir()
    shutil.copytree(source / "es", library)
    module = assets / "engine Ω.mr"
    impulse = assets / "response Ω.wav"
    with wave.open(str(impulse), "wb") as output:
        output.setparams((1, 2, 44100, 0, "NONE", "not compressed"))
        output.writeframes(struct.pack("<hh", 32767, -17))
    recipe = (source / "assets/engines/atg-video-1/02_kohler_ch750.mr").read_text(encoding="utf-8")
    recipe = recipe.replace("ir_lib.default_0", "local_ir").replace("83 * units.mm", "83.125 * units.mm")
    recipe = recipe.replace("impulse_response_library ir_lib()",
                            'impulse_response local_ir(filename: "response Ω.wav")')
    module.write_text(recipe, encoding="utf-8")
    chooser = "choice Ω.mr"
    entry = script_dir / "entry Ω.mr"
    entry.write_bytes(f'import "choice Ω"\r\nmain()\r\n'.encode())
    valid_import = 'public import "engine Ω"\n'
    invalid_import = 'import "missing_decoy.mr"\n'
    (cwd / chooser).write_text(invalid_import, encoding="utf-8")

    def run(label, kind, path, *args):
        command = [str(binary), kind, str(path), str(library), str(assets), *map(str, args)]
        result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=35)
        (root / f"{label}.stdout").write_bytes(result.stdout)
        (root / f"{label}.stderr").write_bytes(result.stderr)
        (root / f"{label}.json").write_text(
            json.dumps({"argv": command, "cwd": str(cwd), "exit": result.returncode}, indent=2) + "\n",
            encoding="utf-8",
        )
        if result.returncode or result.stdout or result.stderr:
            message = (result.stdout + result.stderr).decode("utf-8", errors="replace")
            raise ValueError(f"{label}: expected a silent zero exit, got {result.returncode} {message}")

    priority = (script_dir / chooser, library / chooser, assets / chooser)
    for label, chosen in zip(("importer", "library", "assets"), priority):
        for candidate in priority[priority.index(chosen):]:
            candidate.write_text(valid_import if candidate == chosen else invalid_import, encoding="utf-8")
        run(label, "valid", entry, chosen, impulse)
        chosen.unlink()

    (assets / chooser).write_text(valid_import, encoding="utf-8")
    cases = {
        "comment": (b'unknown(1 + 2)\n/* unterminated', "compile", "Unterminated comment"),
        "missing": (b'import "unavailable.mr"\n', "resources", "unavailable.mr"),
        "invalid": (b'\xff\n', "compile", "UTF-8"),
        "invalid-string": (b'unknown("\xc0\xaf")\n', "compile", "UTF-8"),
        "surrogate": (b'unknown("\xed\xa0\x80")\n', "compile", "UTF-8"),
        "nested-syntax": ('import "choice Ω"\ninvalid !!!\n'.encode(), "compile", "error"),
        "overflow": (b'unknown(88888888888888888888888888888)\n', "compile", "Integer literal outside signed range"),
    }
    for label, (data, stage, message) in cases.items():
        fixture = script_dir / (label + ".mr")
        fixture.write_bytes(data)
        run(label, "rejected", fixture, stage, message, entry)
    run("verify-before-parse", "changed", script_dir / "comment.mr")

    (assets / chooser).write_text(valid_import, encoding="utf-8")
    run("recovery", "valid", entry, assets / chooser, impulse)


if __name__ == "__main__":
    main()
