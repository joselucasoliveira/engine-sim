"""Produce and consume a minimal native SDK transfer without sharing source trees."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import platform
import re
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
PRODUCER_RUNNER = {"Linux": ("Linux", "x86_64"), "Windows": ("Windows", "x86_64"), "Darwin": ("macOS", "arm64")}


def sha256(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def execute(argv, cwd, output, records, env=None):
    label = "%03d" % len(records)
    log = output / (label + ".log")
    argv = list(map(str, argv))
    try:
        with log.open("wb") as stream:
            result = subprocess.run(argv, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=1800)
        record = {"argv": argv, "cwd": str(cwd), "exit": result.returncode, "log": log.name, "sha256": sha256(log)}
    except subprocess.TimeoutExpired:
        record = {"argv": argv, "cwd": str(cwd), "exit": None, "timeout_seconds": 1800, "log": log.name, "sha256": sha256(log)}
    except OSError as error:
        log.write_text(str(error) + "\n", encoding="utf-8")
        record = {"argv": argv, "cwd": str(cwd), "exit": None, "error": str(error), "log": log.name, "sha256": sha256(log)}
    records.append(record)
    save(output / "commands.json", records)
    if record["exit"] != 0:
        raise RuntimeError("Command failed; see " + str(log))
    return log.read_text(encoding="utf-8", errors="replace").strip()


def architecture(machine):
    return {"amd64": "x86_64", "aarch64": "arm64"}.get(machine.lower(), machine.lower())


def native_tuple(out, records, hosted=False):
    system, machine = platform.system(), architecture(platform.machine())
    if system not in PRODUCER_RUNNER or machine != PRODUCER_RUNNER[system][1] or ctypes.sizeof(ctypes.c_void_p) != 8 or sys.byteorder != "little":
        raise RuntimeError("Unsupported actual native OS/architecture/word size")
    identity = {"system": system, "architecture": machine, "platform": platform.platform(), "python": sys.version,
                "hosted": hosted, "runner": {key: os.environ.get(key) for key in
                    ("RUNNER_OS", "RUNNER_ARCH", "ImageOS", "ImageVersion", "GITHUB_SHA", "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GITHUB_WORKFLOW_REF", "WindowsSDKVersion", "VSCMD_VER", "VSCMD_ARG_TGT_ARCH", "DEVELOPER_DIR")}}
    if hosted:
        if identity["runner"]["RUNNER_OS"] != PRODUCER_RUNNER[system][0] or not all(identity["runner"][key] for key in ("ImageOS", "ImageVersion", "GITHUB_RUN_ID")):
            raise RuntimeError("Missing actual hosted runner identity")
    source = out / "native-probe.cpp"
    source.write_text("#ifdef _WIN32\n#if !defined(_MSC_VER) || !defined(_M_X64)\n#error Native MSVC x64 required\n#endif\n#endif\nstatic_assert(sizeof(void*)==8, \"64-bit native process required\");\nint main(){return 0;}\n", encoding="utf-8")
    if system == "Windows":
        if os.environ.get("VSCMD_ARG_TGT_ARCH", "").lower() != "x64" or not os.environ.get("WindowsSDKVersion"):
            raise RuntimeError("Native VS 2022 x64 developer environment and Windows SDK required")
        if hosted and not platform.version().startswith("10.0.20348"):
            raise RuntimeError("Expected actual Windows Server 2022 kernel")
        binary = out / "native-probe.exe"
        identity["compiler"] = execute(["cl.exe", "/nologo", "/Bv", "/std:c++17", source, "/Fe" + str(binary), "/Fo" + str(out / "native-probe.obj")], out, out, records)
        if not re.search(r"19\.(?:3|4)\d", identity["compiler"]):
            raise RuntimeError("Expected VS 2022/v143 compiler")
        version = re.search(r"Compiler Version ([0-9.]+) for x64", identity["compiler"])
        if not version: raise RuntimeError("Could not identify actual MSVC x64 version")
        identity["compiler_version"] = version.group(1)
    else:
        compiler = os.environ.get("CXX", "c++")
        identity["compiler"] = execute([compiler, "--version"], out, out, records)
        if system == "Linux":
            identity["distribution"] = dict(line.split("=", 1) for line in Path("/etc/os-release").read_text().splitlines() if "=" in line)
            version = execute([compiler, "-dumpfullversion"], out, out, records)
            identity["compiler_version"] = version
            if hosted and (identity["distribution"].get("ID", "").strip('"') != "ubuntu" or identity["distribution"].get("VERSION_ID", "").strip('"') != "24.04" or not version.startswith("13.")):
                raise RuntimeError("Expected actual Ubuntu 24.04 and GCC 13")
        else:
            identity["xcode"] = execute(["xcodebuild", "-version"], out, out, records)
            identity["sdk"] = execute(["xcrun", "--show-sdk-version"], out, out, records)
            if not identity["xcode"].startswith("Xcode 16.4\n") or (hosted and not platform.mac_ver()[0].startswith("15.")):
                raise RuntimeError("Expected native macOS 15 arm64 with Xcode 16.4")
        binary = out / "native-probe"
        execute([compiler, "-std=c++17", source, "-o", binary], out, out, records)
    execute([binary], out, out, records)
    identity["native_probe_sha256"] = sha256(binary)
    identity["cmake"] = execute(["cmake", "--version"], out, out, records)
    return identity


def inventory(root):
    result = {}
    for path in sorted(root.rglob("*")):
        name = path.relative_to(root).as_posix()
        if path.is_symlink():
            if not path.resolve().is_relative_to(root):
                raise ValueError("Transfer symlink escapes its root")
            result[name] = {"link": os.readlink(path)}
        elif path.is_file():
            result[name] = {"sha256": sha256(path), "bytes": path.stat().st_size}
        elif not path.is_dir():
            raise ValueError("Unsupported transfer file kind")
    return result


def archive(root, target):
    with tarfile.open(target, "w") as stream:
        for path in sorted(root.rglob("*")):
            info = stream.gettarinfo(str(path), path.relative_to(root).as_posix())
            info.uid = info.gid = info.mtime = 0
            info.uname = info.gname = ""
            info.mode &= 0o777
            with path.open("rb") if info.isfile() else open(os.devnull, "rb") as data:
                stream.addfile(info, data if info.isfile() else None)


def safe_parts(name):
    value = PurePosixPath(name)
    if value.is_absolute() or PureWindowsPath(name).drive or "\\" in name or not value.parts or name.rstrip("/") != value.as_posix() or any(part in (".", "..") or ":" in part for part in value.parts):
        raise ValueError("Unsafe archive path")
    return value.parts


def extract(source, destination):
    with tarfile.open(source, "r") as stream:
        members, total = [], 0
        for member in stream:
            total += member.size
            if len(members) >= 20000 or total > 1024 * 1024 * 1024:
                raise ValueError("Transfer exceeds bounded archive capacity")
            member.mode &= 0o777
            members.append(member)
        names, links = set(), set()
        for member in members:
            parts = safe_parts(member.name)
            name = member.name.casefold() if os.name == "nt" else member.name
            if name in names or not (member.isfile() or member.isdir() or member.issym()):
                raise ValueError("Duplicate or unsupported archive member")
            names.add(name)
            if member.issym():
                safe_parts(member.linkname)
                links.add(parts)
        for member in members:
            parts = safe_parts(member.name)
            if any(parts[:count] in links for count in range(1, len(parts))):
                raise ValueError("Archive writes through a symlink")
        destination.mkdir()
        # Every member and link has been validated for all supported platforms.
        # Do not rely on a filter argument absent from the declared Python floor.
        if hasattr(tarfile, "data_filter"):
            stream.extractall(destination, members=members, filter=tarfile.data_filter)
        else:
            stream.extractall(destination, members=members)


def consumer_sources(source, target):
    target.mkdir()
    for origin, name in (("examples/runtime/consumer", "consumer-source"), ("test/runtime/matrix", "matrix-source"),
                         ("test/runtime/reference", "reference-source"), ("examples/runtime/consumer", "offline-source/consumer")):
        (target / name).mkdir(parents=True)
        expected = {"examples/runtime/consumer": ("CMakeLists.txt", "consumer.c", "consumer.cpp"),
                    "test/runtime/matrix": ("CMakeLists.txt", "matrix.c", "matrix.cpp", "matrix.h"),
                    "test/runtime/reference": ("CMakeLists.txt", "direct.c")}[origin]
        for filename in expected:
            shutil.copyfile(source / origin / filename, target / name / filename)
    for origin, name in (("examples/runtime/CMakeLists.txt", "offline-source/CMakeLists.txt"),
                         ("examples/runtime/render.cpp", "offline-source/render.cpp"), ("test/runtime/example_tests.py", "example_tests.py"),
                         ("examples/runtime/ffi_smoke.py", "ffi_smoke.py"), ("test/runtime/ffi_matrix.py", "ffi_matrix.py")):
        shutil.copyfile(source / origin, target / name)


def produce(args, records, identity):
    source = args.source.resolve()
    revision = execute(["git", "rev-parse", "HEAD"], source, args.out, records)
    dirty = execute(["git", "status", "--porcelain", "--untracked-files=all"], source, args.out, records)
    if revision != args.revision or (dirty and not args.allow_dirty):
        raise ValueError("Producer differs from requested clean revision")
    build = args.out / "build"
    env = dict(os.environ, ES_OUT=str(build), CMAKE_GENERATOR="Ninja")
    if args.deps_root:
        deps = args.deps_root / "deps.cmake"
        receipt = args.deps_root / "dependency-receipt.json"
        if json.loads(receipt.read_text())["lock_sha256"] != sha256(source / "cmake/dependencies.lock.json"):
            raise ValueError("Prepared dependency lock differs")
    else:
        argv = [sys.executable, "-B", source / "tools/bootstrap.py", "--profile", "runtime", "--out", build]
        if args.cache: argv += ["--cache", args.cache]
        if args.offline: argv += ["--offline"]
        execute(argv, source, args.out, records, env)
        deps, receipt = build / "deps.cmake", build / "dependency-receipt.json"
    preset = "runtime-" + args.variant
    execute(["cmake", "--preset", preset, "-DENGINE_SIM_INSTALL_RUNTIME_EXAMPLE=ON", "-DENGINE_SIM_BUILD_RUNTIME_EXAMPLES=ON",
             "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DENGINE_SIM_DEPS_FILE=" + str(deps)], source, args.out, records, env)
    execute(["cmake", "--build", "--preset", preset, "--config", "Release", "--parallel", "2"], source, args.out, records, env)
    prefix = args.out / "prefix"
    execute(["cmake", "--install", build / preset, "--config", "Release", "--prefix", prefix, "--component", "RuntimeSDK"], source, args.out, records, env)
    payload = args.out / "transfer"; payload.mkdir()
    inputs = args.out / "consumer-inputs"; consumer_sources(source, inputs)
    archive(prefix, payload / "sdk.tar")
    archive(inputs, payload / "consumers.tar")
    shutil.copyfile(__file__, payload / "runtime_ci.py")
    shutil.copyfile(source / "test/runtime/installed_consumer.py", payload / "installed_consumer.py")
    manifest = {"version": 1, "revision": revision, "variant": args.variant, "dirty": bool(dirty), "native": identity,
                "dependency_receipt": json.loads(receipt.read_text()), "lock_sha256": sha256(source / "cmake/dependencies.lock.json"),
                "producer_absence_checks": [str(source / name) for name in (".git", "CMakeLists.txt", "include/engine_sim/runtime.h")] + [str(build / preset / "CMakeCache.txt")],
                "sdk_files": inventory(prefix), "consumer_files": inventory(inputs),
                "files": {path.name: sha256(path) for path in sorted(payload.iterdir())}}
    save(payload / "manifest.json", manifest)
    return {"status": "pass", "role": "producer", "revision": revision, "dirty": bool(dirty), "native": identity,
            "transfer_manifest_sha256": sha256(payload / "manifest.json"), "transfer": str(payload)}


def consume(args, records, identity):
    payload = args.payload.resolve()
    manifest = json.loads((payload / "manifest.json").read_text())
    if manifest["version"] != 1 or manifest["revision"] != args.revision or manifest["variant"] != args.variant or (manifest["dirty"] and not args.allow_dirty):
        raise ValueError("Transfer revision/variant/clean identity differs")
    if (manifest["native"]["system"], manifest["native"]["architecture"]) != (identity["system"], identity["architecture"]):
        raise ValueError("Producer and consumer native tuples differ")
    if manifest["native"]["hosted"] != args.hosted:
        raise ValueError("Producer and consumer qualification scopes differ")
    if (identity["system"] != "Windows" and manifest["native"]["compiler"] != identity["compiler"]) or any(manifest["native"].get(key) != identity.get(key) for key in ("compiler_version", "xcode", "sdk")):
        raise ValueError("Producer and consumer compiler/SDK identities differ")
    if identity["system"] == "Windows" and manifest["native"]["runner"]["WindowsSDKVersion"] != identity["runner"]["WindowsSDKVersion"]:
        raise ValueError("Producer and consumer Windows SDKs differ")
    if args.hosted and manifest["native"]["runner"]["ImageVersion"] != identity["runner"]["ImageVersion"]:
        raise ValueError("Producer and consumer runner images differ")
    if args.hosted and manifest["native"]["runner"]["GITHUB_RUN_ID"] != identity["runner"]["GITHUB_RUN_ID"]:
        raise ValueError("Transfer came from another workflow run")
    if set(manifest["files"]) != {"sdk.tar", "consumers.tar", "runtime_ci.py", "installed_consumer.py"}:
        raise ValueError("Unexpected transfer inventory")
    for name, value in manifest["files"].items():
        if sha256(payload / name) != value: raise ValueError("Transfer file identity differs: " + name)
    if any(Path(name).exists() for name in manifest["producer_absence_checks"]):
        raise ValueError("Producer source/build tree is still accessible to the consumer")
    work = args.out / "work"
    extract(payload / "consumers.tar", work)
    if inventory(work) != manifest["consumer_files"]: raise ValueError("Consumer input identity differs")
    sdk = work / "SDK espaço Ω"
    extract(payload / "sdk.tar", sdk)
    if inventory(sdk) != manifest["sdk_files"]: raise ValueError("Installed SDK identity differs")
    execute([sys.executable, "-B", payload / "installed_consumer.py", "--prepared", "--prefix", sdk, "--variant", args.variant, "--out", work], work, args.out, records)
    result = json.loads((work / "result.json").read_text())
    if result["status"] != "pass": raise ValueError("Installed consumer failed")
    return {"status": "pass", "role": "consumer", "revision": args.revision, "variant": args.variant,
            "native": identity, "producer_trees_absent": True, "manifest_sha256": sha256(payload / "manifest.json"),
            "installed_result_sha256": sha256(work / "result.json"), "commands": result["commands"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("role", choices=("produce", "consume", "probe"))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--variant", choices=("static", "shared"), required=True)
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--payload", type=Path)
    parser.add_argument("--deps-root", type=Path)
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--hosted", action="store_true")
    parser.add_argument("--allow-dirty", action="store_true")
    args = parser.parse_args()
    if args.role == "consume" and not args.payload:
        parser.error("Clean consumption requires a transferred payload")
    if not re.fullmatch("[0-9a-f]{40}", args.revision) or not args.out.is_absolute() or args.out.exists():
        parser.error("Use a full revision and a fresh absolute output")
    for path in (args.source, args.payload, args.deps_root, args.cache):
        if path is not None and not path.is_absolute(): parser.error("All supplied paths must be absolute")
    if args.out.resolve().is_relative_to(args.payload.resolve() if args.role == "consume" else args.source.resolve()):
        parser.error("Output must be separate from source/payload")
    args.out.mkdir(parents=True)
    records = []
    try:
        identity = native_tuple(args.out, records, args.hosted)
        if args.role == "probe":
            actual = execute(["git", "rev-parse", "HEAD"], args.source, args.out, records)
            dirty = execute(["git", "status", "--porcelain", "--untracked-files=all"], args.source, args.out, records)
            if actual != args.revision or (dirty and not args.allow_dirty): raise ValueError("Diagnostic source differs from requested revision")
            result = {"status": "pass", "role": "probe", "revision": args.revision, "native": identity}
        else:
            result = produce(args, records, identity) if args.role == "produce" else consume(args, records, identity)
    except Exception as error:
        result = {"status": "fail", "role": args.role, "error": str(error), "revision": args.revision, "variant": args.variant}
    result["driver_commands"] = records
    save(args.out / "result.json", result)
    print(json.dumps({key: value for key, value in result.items() if key not in ("commands", "driver_commands")}, indent=2))
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
