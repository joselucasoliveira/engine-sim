"""Verify and stage the allowlisted runtime data without acquiring new inputs."""

import argparse
import hashlib
import json
import platform
from pathlib import Path
import shutil
import sys


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def external(path, source):
    path = path.resolve()
    if path.is_relative_to(source):
        raise ValueError(f"staging must be outside the source checkout: {path}")
    return path


def records(options):
    manifest = json.loads(options.manifest.read_text())
    if manifest.get("schema") != 1:
        raise ValueError("unsupported runtime allowlist schema")
    roots = {"project": options.source, "delta": options.delta}
    if any(row["component"] not in {"base", "desktop"} for row in manifest["files"]):
        raise ValueError("unknown runtime component")
    selected = [row for row in manifest["files"] if (row["component"] == "base" or options.desktop)
                and options.platform in row.get("platforms", [options.platform])]
    destinations = set()
    checked = []
    recordings = []
    models = set()
    required_impulses = set()

    def check_input(row, root, relative, destination):
        source = (root / relative).resolve()
        if not source.is_relative_to(root) or not source.is_file():
            raise ValueError(f"missing allowlisted runtime input: {source}")
        data = source.read_bytes()
        if hashlib.sha256(data).hexdigest() != row["sha256"]:
            raise ValueError(f"runtime input differs from its recorded identity: {source}")
        if row["distribution"] not in {"permitted", "local-validation-only"}:
            raise ValueError(f"unrecognized distribution disposition: {source}")
        if options.desktop and row.get("model"):
            models.add(destination.as_posix())
            required_impulses.update(item["path"] for item in json.loads(data)["components"]["impulses"])
        checked.append((row, source, destination))

    for row in selected:
        root = roots[row["source"]].resolve()
        relative = Path(row["path"])
        destination = Path(row["destination"])
        if relative.is_absolute() or destination.is_absolute() or ".." in relative.parts or ".." in destination.parts:
            raise ValueError("runtime allowlist contains a non-relative path")
        if options.desktop and (relative.suffix.lower() == ".mr" or destination.suffix.lower() == ".mr"):
            continue
        if destination.as_posix() in destinations:
            raise ValueError(f"duplicate runtime destination: {destination}")
        destinations.add(destination.as_posix())
        entry = (row, root, relative, destination)
        if options.desktop and destination.suffix.lower() == ".wav":
            recordings.append(entry)
        else:
            check_input(*entry)
    if options.desktop:
        if "assets/main.json" not in models:
            raise ValueError("the desktop allowlist must contain the canonical default model")
        absent = required_impulses - {entry[3].as_posix() for entry in recordings}
        if absent:
            raise ValueError("desktop model impulse responses are absent from the allowlist: " + ", ".join(sorted(absent)))
        for entry in recordings:
            if entry[3].as_posix() in required_impulses:
                check_input(*entry)
    pending = [row["path"] for row, _, _ in checked if row["distribution"] == "local-validation-only"]
    if pending and not options.local_validation:
        raise ValueError("redistribution staging is blocked by unresolved impulse-recording clearance: "
                         + ", ".join(pending) + ". Use the explicit local-validation mode for a private copy.")
    return checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--delta", type=Path)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--desktop", action="store_true")
    parser.add_argument("--platform", default=platform.system())
    parser.add_argument("--local-validation", action="store_true")
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--inventory", action="store_true")
    options = parser.parse_args()
    options.source = options.source.resolve()
    options.delta = options.delta or options.source / "dependencies/submodules/delta-studio"
    options.destination = external(options.destination, options.source)
    if options.desktop and any(path.suffix.lower() == ".mr" for path in options.destination.rglob("*")
                               if path.is_file() or path.is_symlink()):
        raise ValueError("use a script-free desktop staging destination")
    if options.inventory:
        if any(options.destination.rglob("LOCAL_VALIDATION_ONLY.txt")):
            options.local_validation = True
        records(options)
        files = {file.relative_to(options.destination).as_posix(): {"bytes": file.stat().st_size, "sha256": digest(file)}
                 for file in sorted(options.destination.rglob("*")) if file.is_file() and file.name != "runtime-inventory.json"}
        inventory = options.destination / "runtime-inventory.json"
        inventory.write_text(json.dumps({"schema": 1, "mode": "local-validation" if options.local_validation else "redistributable",
                                         "files": files}, indent=2, sort_keys=True) + "\n")
        print(f"recorded {len(files)} installed files")
        return
    checked = records(options)
    if options.check_only:
        print(f"verified {len(checked)} allowlisted runtime inputs")
        return
    options.destination.mkdir(parents=True, exist_ok=True)
    for row, source, relative in checked:
        destination = options.destination / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        if digest(destination) != row["sha256"]:
            raise ValueError(f"copied runtime input has unexpected bytes: {destination}")
    if options.local_validation:
        (options.destination / "LOCAL_VALIDATION_ONLY.txt").write_text(
            "Private local validation copy. Includes original impulse recordings whose separate redistribution "
            "clearance has not been established. This tree is not a cleared distribution.\n")
    print(f"staged {len(checked)} verified runtime inputs")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, AssertionError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
