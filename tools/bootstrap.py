#!/usr/bin/env python3
"""Verify and prepare dependencies in an external Engine Simulator build directory."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

REPO = Path(__file__).resolve().parent.parent
LOCK_PATH = REPO / "cmake" / "dependencies.lock.json"


def command(*args, cwd=REPO, capture=False):
    print("+", subprocess.list2cmdline([str(arg) for arg in args]), flush=True)
    result = subprocess.run(args, cwd=cwd, check=True, text=True, capture_output=capture)
    return result.stdout.strip() if capture else None


def external_path(value):
    path = Path(value)
    if not path.is_absolute():
        raise argparse.ArgumentTypeError(f"expected an absolute path: {path}")
    real = path.resolve()
    if real.is_relative_to(REPO):
        raise argparse.ArgumentTypeError(f"path is inside the source checkout: {path}")
    return real


def file_digest(path):
    with path.open("rb") as stream:
        digest = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
        return digest.hexdigest()


def verify_archive(path, entry):
    actual = file_digest(path)
    expected = entry["sha256"]
    if actual != expected:
        raise ValueError(f"{entry['filename']}: SHA-256 expected {expected}, got {actual}")
    if "bytes" in entry and path.stat().st_size != entry["bytes"]:
        raise ValueError(f"{entry['filename']}: expected {entry['bytes']} bytes")


def archive(cache, entry, offline):
    target = cache / entry["filename"]
    if target.is_file():
        verify_archive(target, entry)
        return target
    if offline:
        raise ValueError(f"missing offline archive {target}, SHA-256 {entry['sha256']}")
    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=cache) as directory:
        pending = Path(directory) / target.name
        print(f"Downloading {entry['url']}", flush=True)
        with urllib.request.urlopen(entry["url"], timeout=60) as source, pending.open("wb") as output:
            shutil.copyfileobj(source, output)
        verify_archive(pending, entry)
        pending.replace(target)
    return target


def member_path(root, name):
    member = PurePosixPath(name)
    if member.is_absolute() or not member.parts or ".." in member.parts or ":" in name or "\\" in name:
        raise ValueError(f"invalid archive path {name!r}")
    path = root.joinpath(*member.parts).resolve()
    if not path.is_relative_to(root):
        raise ValueError(f"archive path escapes staging: {name!r}")
    return path


def unpack(source, root):
    if source.name.endswith(".tar.gz"):
        with tarfile.open(source, "r:gz") as contents:
            for item in contents:
                target = member_path(root, item.name)
                if item.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                elif item.isfile():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with contents.extractfile(item) as stream, target.open("wb") as output:
                        shutil.copyfileobj(stream, output)
                    target.chmod(item.mode & 0o777)
                else:
                    raise ValueError(f"unsupported archive member {item.name!r}")
    else:
        with zipfile.ZipFile(source) as contents:
            for item in contents.infolist():
                target = member_path(root, item.filename)
                if item.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                mode = item.external_attr >> 16
                if stat.S_ISLNK(mode):
                    raise ValueError(f"archive member is a symlink: {item.filename!r}")
                target.parent.mkdir(parents=True, exist_ok=True)
                with contents.open(item) as stream, target.open("wb") as output:
                    shutil.copyfileobj(stream, output)
                if item.create_system == 3 and mode & 0o777:
                    target.chmod(mode & 0o777)


def verify_submodules(names, git):
    revisions = {}
    for name in names:
        path = REPO / "dependencies" / "submodules" / name
        expected = git[name]["revision"]
        if not (path / ".git").exists():
            raise ValueError(f"missing {name} at {expected}; run git submodule update --init --recursive")
        revision = command("git", "rev-parse", "HEAD", cwd=path, capture=True)
        if revision != expected:
            raise ValueError(f"{name}: expected {expected}, found {revision}")
        if command("git", "--no-optional-locks", "status", "--porcelain", "--untracked-files=all",
                   cwd=path, capture=True):
            raise ValueError(f"{name} has local modifications")
        revisions[name] = revision
    return revisions


def prepare_boost(source):
    windows = platform.system() == "Windows"
    if windows:
        command("cmd.exe", "/d", "/c", "bootstrap.bat", cwd=source)
        options = ("toolset=msvc-14.3", "architecture=x86", "address-model=64", "variant=debug,release")
        b2 = source / "b2.exe"
    else:
        script = source / "tools" / "build" / "src" / "engine" / "build.sh"
        script.chmod(script.stat().st_mode | stat.S_IXUSR)
        command("/bin/sh", "./bootstrap.sh", "--with-libraries=filesystem", cwd=source)
        options = ("variant=release", "cxxflags=-fPIC")
        b2 = source / "b2"
    command(b2, "--with-filesystem", *options, "link=static", "runtime-link=shared",
            "threading=multi", "-j4", "stage", cwd=source)
    return {
        "BOOST_ROOT": source,
        "BOOST_LIBRARYDIR": source / "stage" / "lib",
        "Boost_NO_SYSTEM_PATHS": "ON",
    }


def find_file(root, name, architecture=None):
    matches = [
        path for path in root.rglob(name)
        if path.is_file() and (architecture is None or architecture in path.parts)
    ]
    if len(matches) != 1:
        raise ValueError(f"expected one {name} in {root}, found {matches}")
    return matches[0]


def prepare_windows(paths):
    flex = paths["winflexbison"]
    values = {
        "FLEX_EXECUTABLE": find_file(flex, "win_flex.exe"),
        "BISON_EXECUTABLE": find_file(flex, "win_bison.exe"),
    }
    for name in ("FLEX_EXECUTABLE", "BISON_EXECUTABLE"):
        command(values[name], "--version", cwd=values[name].parent)
    if "sdl2" not in paths:
        return values
    sdl, image, dx = (paths[name] for name in ("sdl2", "sdl2-image", "dxsdk"))
    sdl_library = find_file(sdl, "SDL2.lib", "x64")
    return {
        **values,
        "SDL2_INCLUDE_DIR": sdl / "include",
        "SDL2_LIBRARY": sdl_library,
        "SDL2_LIBRARY_TEMP": sdl_library,
        "SDL2_IMAGE_INCLUDE_DIR": image / "include",
        "SDL2_IMAGE_LIBRARY": find_file(image, "SDL2_image.lib", "x64"),
        "ENGINE_SIM_SDL2_DLL": find_file(sdl, "SDL2.dll", "x64"),
        "ENGINE_SIM_SDL2_IMAGE_DLL": find_file(image, "SDL2_image.dll", "x64"),
        "ENGINE_SIM_D3DX10_DLL": find_file(dx / "build/native/release", "d3dx10_43.dll", "x64"),
        "ENGINE_SIM_D3DX11_DLL": find_file(dx / "build/native/release", "d3dx11_43.dll", "x64"),
        "ENGINE_SIM_D3DCOMPILER_DLL": find_file(dx / "build/native/release", "D3DCompiler_43.dll", "x64"),
        "ENGINE_SIM_D3DX_NOTICE": dx / "LICENSE.txt",
    }


def cmake_value(value):
    literal = value.as_posix() if isinstance(value, Path) else value
    delimiter = ""
    while f"]{delimiter}]" in literal:
        delimiter += "="
    return f"[{delimiter}[{literal}]{delimiter}]"


def write_atomic(path, text):
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent, delete=False) as stream:
        stream.write(text)
        temp = Path(stream.name)
    temp.replace(path)


def bootstrap(options):
    windows = platform.system() == "Windows"
    lock = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
    names = ["simple-2d-constraint-solver"]
    if options.profile not in {"runtime", "converter"}:
        names.append("csv-io")
    inputs = ["googletest"] if options.tests else []
    if options.profile != "core":
        names.append("piranha")
        inputs.append("boost")
        if windows:
            inputs.append("winflexbison")
    if options.profile == "desktop":
        if windows:
            names.append("delta-studio")
            inputs.extend(("sdl2", "sdl2-image", "dxsdk"))
        else:
            inputs.append("sdl3")

    revisions = verify_submodules(names, lock["git"])
    cache = options.cache or options.out / "cache"
    files = {name: archive(cache, lock["archives"][name], options.offline) for name in inputs}
    options.out.mkdir(parents=True, exist_ok=True)
    prepared = Path(tempfile.mkdtemp(prefix="deps-", dir=options.out))
    try:
        paths = {}
        for name, path in files.items():
            target = prepared / name
            target.mkdir()
            unpack(path, target)
            paths[name] = target / lock["archives"][name]["root"]

        values = {}
        if "googletest" in paths:
            values["ENGINE_SIM_GOOGLETEST_SOURCE"] = paths["googletest"]
        if "boost" in paths:
            values.update(prepare_boost(paths["boost"]))
        if "winflexbison" in paths:
            values.update(prepare_windows(paths))
        if "sdl3" in paths:
            values["ENGINE_SIM_SDL3_SOURCE"] = paths["sdl3"]

        receipt = {
            "profile": options.profile,
            "tests": options.tests,
            "offline": options.offline,
            "host": f"{platform.system()} {platform.machine()}",
            "revision": command("git", "rev-parse", "HEAD", capture=True),
            "lock_sha256": file_digest(LOCK_PATH),
            "git": revisions,
            "archives": {name: {"path": str(path), "sha256": lock["archives"][name]["sha256"]}
                         for name, path in files.items()},
            "cmake": {name: str(value) for name, value in values.items()},
        }
        write_atomic(options.out / "dependency-receipt.json", json.dumps(receipt, indent=2) + "\n")
        write_atomic(options.out / "deps.cmake", "".join(
            f"set({name} {cmake_value(value)} CACHE STRING \"Verified bootstrap input\" FORCE)\n"
            for name, value in values.items()))
    except (OSError, ValueError, subprocess.CalledProcessError, tarfile.TarError, zipfile.BadZipFile):
        shutil.rmtree(prepared, ignore_errors=True)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("core", "capture", "desktop", "runtime", "converter"), required=True)
    parser.add_argument("--out", type=external_path, required=True)
    parser.add_argument("--cache", type=external_path)
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--tests", action="store_true")
    try:
        bootstrap(parser.parse_args())
    except (OSError, ValueError, subprocess.CalledProcessError, tarfile.TarError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Bootstrap failed: {error}\n")


if __name__ == "__main__":
    main()
