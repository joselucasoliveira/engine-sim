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
SOLVER = "simple-2d-constraint-solver"
ACQUISITION = {
    "modules": (SOLVER,), "converter": (SOLVER, "piranha"), "minimal": (SOLVER,),
    "combined": (SOLVER, "piranha"), "desktop": (SOLVER,), "diagnostics": (SOLVER,),
}
SEARCH_ENV = ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "CMAKE_PREFIX_PATH",
              "CMAKE_LIBRARY_PATH", "CMAKE_INCLUDE_PATH", "CMAKE_MODULE_PATH", "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR")
OPTIONAL_TARGET = re.compile(r"piranha|delta|converter|engine-sim-(?:app|capture|host|frontend|desktop)|sdl|boost|discord|googletest|direct-to-video", re.I)


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


def acquisition_paths(profile, system):
    names = ACQUISITION[profile]
    if system == "Windows" and profile in ("desktop", "combined"):
        names += ("delta-studio",)
    return [f"dependencies/submodules/{name}" for name in names]


def deployment_options(system):
    if system == "Darwin":
        return ["-DCMAKE_OSX_ARCHITECTURES=arm64", "-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0"]
    return []


def sdk_plan(variant, profile, system):
    overlays = []
    options = []
    targets = ["engine-sim-runtime"]
    if profile == "combined":
        overlays = ["--with-desktop", "--with-capture", "--with-converter"]
        options += ["-DENGINE_SIM_BUILD_DESKTOP=ON", "-DENGINE_SIM_BUILD_CAPTURE=ON",
                    "-DENGINE_SIM_BUILD_LEGACY_CONVERTER=ON"]
        targets += ["engine-sim-app", "engine-sim-capture", "engine-sim-convert"]
        if variant == "static":
            overlays.append("--tests")
            options += ["-DBUILD_TESTING=ON", "-DENGINE_SIM_BUILD_HOST_TESTS=ON"]
            targets += ["engine-sim-host-test", "engine-sim-pacing-test"]
    return overlays, options + deployment_options(system), targets


def sdk_targets(build):
    reply = build / ".cmake/api/v1/reply"
    model = json.loads(next(reply.glob("codemodel-v2-*.json")).read_text(encoding="utf-8"))
    targets = {target["id"]: target for target in model["configurations"][0]["targets"]}
    edges = {}
    queue = [next(id for id, target in targets.items() if target["name"] == "engine-sim-runtime")]
    while queue:
        target = targets[queue.pop()]
        name = target["name"]
        if name in edges:
            continue
        details = json.loads((reply / target["jsonFile"]).read_text(encoding="utf-8"))
        dependencies = [row["id"] for row in details.get("dependencies", [])]
        edges[name] = sorted(targets[id]["name"] for id in dependencies)
        queue.extend(dependencies)
    if OPTIONAL_TARGET.search(" ".join(edges)) or not {"engine-sim-config", "engine-sim-json", SOLVER} <= edges.keys():
        raise ValueError("Unexpected SDK link target closure")
    return edges


def dependency_identity(receipt, lock, profile, variant, system):
    paths = acquisition_paths(profile, system)
    archives = set()
    if profile == "combined":
        archives = {"boost", "winflexbison", "sdl2", "sdl2-image", "dxsdk"} if system == "Windows" else {"boost", "sdl3"}
        if variant == "static":
            archives.add("googletest")
    if (receipt["lock_sha256"] != lock or receipt["profile"] != "runtime"
            or set(receipt["git"]) != {path.rsplit("/", 1)[1] for path in paths}
            or set(receipt["archives"]) != archives):
        raise ValueError("SDK acquisition selection disagrees with the qualification profile")


def compare_results(minimal, combined):
    rows = []
    for root, profile in ((minimal, "minimal"), (combined, "combined")):
        result = json.loads((root / "result.json").read_text(encoding="utf-8"))
        manifest_file = root / "transfer-manifest.json"
        manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
        installed_file = root / "work/result.json"
        installed = json.loads(installed_file.read_text(encoding="utf-8"))
        if (result["status"] != "pass" or result["role"] != "consumer" or not result["producer_trees_absent"]
                or result["profile"] != profile or manifest["profile"] != profile
                or result["manifest_sha256"] != sha256(manifest_file)
                or result["installed_result_sha256"] != sha256(installed_file)
                or installed["status"] != "pass"):
            raise ValueError("Consumer qualification receipt differs")
        rows.append((manifest, installed))
    (left, left_proof), (right, right_proof) = rows
    for key in ("revision", "variant", "dirty", "lock_sha256", "consumer_files", "sdk_targets"):
        if left[key] != right[key]:
            raise ValueError("Combined SDK differs in " + key)
    if left["sdk_files"].keys() != right["sdk_files"].keys():
        raise ValueError("Combined SDK path inventory differs")
    for key in ("system", "architecture", "hosted", "compiler_version", "sdk", "xcode"):
        if left["native"].get(key) != right["native"].get(key):
            raise ValueError("Compared native toolchains differ in " + key)
    for key in ("WindowsSDKVersion", "ImageVersion", "GITHUB_RUN_ID"):
        if left["native"]["runner"].get(key) != right["native"]["runner"].get(key):
            raise ValueError("Compared native runner identities differ in " + key)
    for key in ("native_matrix", "foreign_matrix"):
        expected_count = int(key == "native_matrix" or left["variant"] == "shared")
        expected = [row[key] for row in left_proof["commands"] if key in row]
        actual = [row[key] for row in right_proof["commands"] if key in row]
        if len(expected) != expected_count or expected != actual:
            raise ValueError("Combined SDK output differs in " + key)
    differences = [key for key in left["sdk_files"] if left["sdk_files"][key] != right["sdk_files"][key]]
    if any(not re.search(r"\.(?:a|lib|dll|pdb|dylib|so)(?:\.\d+)*$", key, re.I) for key in differences):
        raise ValueError("Combined SDK headers or imported target metadata differ")
    return {"status": "pass", "revision": left["revision"], "variant": left["variant"],
            "compared_sdk_paths": len(left["sdk_files"]), "binary_paths_with_different_bytes": differences}


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
    fixture = target / "fixture"
    fixture.mkdir()
    allowlist = json.loads((source / "cmake/runtime-example.json").read_text(encoding="utf-8"))
    for item in allowlist["files"]:
        parts = safe_parts(item["path"])
        if len(parts) != 1:
            raise ValueError("Unexpected fixture path: " + item["path"])
        origin = source / "examples/runtime/fixture" / item["path"]
        if origin.is_symlink() or sha256(origin) != item["sha256"]:
            raise ValueError("CI fixture differs from the licensed allowlist: " + item["path"])
        shutil.copyfile(origin, fixture / origin.name)


def source_identity(args, records):
    source = args.source.resolve()
    revision = execute(["git", "rev-parse", "HEAD"], source, args.out, records)
    dirty = execute(["git", "status", "--porcelain", "--untracked-files=all"], source, args.out, records)
    if revision != args.revision or (dirty and not args.allow_dirty):
        raise ValueError("Source differs from requested revision")
    return revision, bool(dirty)


def produce(args, records, identity):
    source = args.source.resolve()
    revision, dirty = source_identity(args, records)
    overlays, options, targets = sdk_plan(args.variant, args.profile, identity["system"])
    build = args.out / "build"
    env = {key: value for key, value in os.environ.items() if key not in SEARCH_ENV}
    home = args.out / "empty-home"; home.mkdir()
    env.update(ES_OUT=str(build), CMAKE_GENERATOR="Ninja", HOME=str(home))
    if args.deps_root:
        deps = args.deps_root / "deps.cmake"
        receipt = args.deps_root / "dependency-receipt.json"
        if json.loads(receipt.read_text())["lock_sha256"] != sha256(source / "cmake/dependencies.lock.json"):
            raise ValueError("Prepared dependency lock differs")
    else:
        argv = [sys.executable, "-B", source / "tools/bootstrap.py", "--profile", "runtime", "--out", build, *overlays]
        if args.cache: argv += ["--cache", args.cache]
        if args.offline: argv += ["--offline"]
        execute(argv, source, args.out, records, env)
        deps, receipt = build / "deps.cmake", build / "dependency-receipt.json"
    preset = "runtime-" + args.variant
    lock = sha256(source / "cmake/dependencies.lock.json")
    receipt_data = json.loads(receipt.read_text(encoding="utf-8"))
    dependency_identity(receipt_data, lock, args.profile, args.variant, identity["system"])
    query = build / preset / ".cmake/api/v1/query"
    query.mkdir(parents=True)
    (query / "codemodel-v2").touch()
    execute(["cmake", "--preset", preset, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DENGINE_SIM_DEPS_FILE=" + str(deps),
             "-DFETCHCONTENT_FULLY_DISCONNECTED=ON", "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
             "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF", *options], source, args.out, records, env)
    execute(["cmake", "--build", "--preset", preset, "--config", "Release", "--parallel", str(args.jobs),
             "--verbose", "--target", *targets], source, args.out, records, env)
    closure = sdk_targets(build / preset)
    if "--tests" in overlays:
        execute(["ctest", "--test-dir", build / preset, "-C", "Release", "--no-tests=error", "--output-on-failure",
                 "--output-junit", args.out / "host-tests.xml", "-R", r"^((HostContract|RuntimeDesktop)\.|CaptureCanonicalReplay$|CaptureNoLegacyAdapter$)"],
                source, args.out, records, env)
    prefix = args.out / "prefix"
    execute(["cmake", "--install", build / preset, "--config", "Release", "--prefix", prefix, "--component", "RuntimeSDK"], source, args.out, records, env)
    payload = args.out / "transfer"; payload.mkdir()
    inputs = args.out / "consumer-inputs"; consumer_sources(source, inputs)
    archive(prefix, payload / "sdk.tar")
    archive(inputs, payload / "consumers.tar")
    shutil.copyfile(__file__, payload / "runtime_ci.py")
    shutil.copyfile(source / "test/runtime/installed_consumer.py", payload / "installed_consumer.py")
    manifest = {"version": 1, "revision": revision, "variant": args.variant, "profile": args.profile, "dirty": dirty, "native": identity,
                "dependency_receipt": receipt_data, "lock_sha256": lock,
                "producer_absence_checks": [str(source / name) for name in (".git", "CMakeLists.txt", "include/engine_sim/runtime.h")] + [str(build / preset / "CMakeCache.txt")],
                "sdk_files": inventory(prefix), "consumer_files": inventory(inputs), "sdk_targets": closure,
                "files": {path.name: sha256(path) for path in sorted(payload.iterdir())}}
    save(payload / "manifest.json", manifest)
    return {"status": "pass", "role": "producer", "revision": revision, "profile": args.profile, "dirty": dirty, "native": identity,
            "transfer_manifest_sha256": sha256(payload / "manifest.json"), "transfer": str(payload)}


def consume(args, records, identity):
    payload = args.payload.resolve()
    manifest = json.loads((payload / "manifest.json").read_text())
    if manifest["version"] != 1 or manifest["revision"] != args.revision or manifest["variant"] != args.variant or (manifest["dirty"] and not args.allow_dirty):
        raise ValueError("Transfer revision/variant/clean identity differs")
    if manifest.get("profile", "minimal") != args.profile:
        raise ValueError("Transfer producer profile differs")
    dependency_identity(manifest["dependency_receipt"], manifest["lock_sha256"], args.profile, args.variant, identity["system"])
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
    hidden = not any(Path(name).exists() for name in manifest["producer_absence_checks"])
    if not hidden and not args.relocation_only:
        raise ValueError("Producer source/build tree is still accessible to the consumer")
    work = args.out / "work"
    extract(payload / "consumers.tar", work)
    if inventory(work) != manifest["consumer_files"]: raise ValueError("Consumer input identity differs")
    sdk = work / "SDK espaço Ω"
    extract(payload / "sdk.tar", sdk)
    if inventory(sdk) != manifest["sdk_files"]: raise ValueError("Installed SDK identity differs")
    fixture = ["--fixture", work / "fixture"] if (work / "fixture").is_dir() else []
    execute([sys.executable, "-B", payload / "installed_consumer.py", "--prepared", "--prefix", sdk, "--variant", args.variant, "--out", work, *fixture], work, args.out, records)
    result = json.loads((work / "result.json").read_text())
    if result["status"] != "pass": raise ValueError("Installed consumer failed")
    shutil.copyfile(payload / "manifest.json", args.out / "transfer-manifest.json")
    return {"status": "pass", "role": "consumer", "revision": args.revision, "variant": args.variant, "profile": args.profile,
            "native": identity, "producer_trees_absent": hidden, "manifest_sha256": sha256(payload / "manifest.json"),
            "installed_result_sha256": sha256(work / "result.json"), "commands": result["commands"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("role", choices=("produce", "consume", "probe", "acquire", "profiles", "compare"))
    parser.add_argument("--out", type=Path)
    parser.add_argument("--revision")
    parser.add_argument("--variant", choices=("static", "shared"), default="static")
    parser.add_argument("--profile", choices=ACQUISITION, default="minimal")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--payload", type=Path)
    parser.add_argument("--deps-root", type=Path)
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--combined", type=Path)
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--hosted", action="store_true")
    parser.add_argument("--allow-dirty", action="store_true")
    parser.add_argument("--relocation-only", action="store_true")
    args = parser.parse_args()
    if args.role == "compare":
        if not all(path and path.is_absolute() for path in (args.payload, args.combined, args.out)) or args.out.exists():
            parser.error("Use absolute minimal and combined consumer result paths and a fresh output file")
        try:
            result = compare_results(args.payload, args.combined)
        except (OSError, KeyError, ValueError) as error:
            result = {"status": "fail", "error": str(error)}
        save(args.out, result)
        print(json.dumps(result, indent=2))
        return int(result["status"] != "pass")
    if args.role == "acquire":
        paths = acquisition_paths(args.profile, platform.system())
        return subprocess.call(["git", "submodule", "update", "--init", "--", *paths], cwd=args.source)
    if args.role in ("produce", "consume") and args.profile not in ("minimal", "combined"):
        parser.error("Use a minimal or combined SDK profile")
    if args.role == "profiles" and args.profile not in ("modules", "converter"):
        parser.error("Use the modules or converter profile")
    if args.role == "consume" and not args.payload:
        parser.error("Clean consumption requires a transferred payload")
    if args.relocation_only and (args.role != "consume" or args.hosted or platform.system() == "Linux"):
        parser.error("Relocation-only consumption is for local Windows/macOS checks")
    if not re.fullmatch("[0-9a-f]{40}", args.revision or "") or not args.out or not args.out.is_absolute() or args.out.exists() or args.jobs < 1:
        parser.error("Use a full revision and a fresh absolute output")
    for path in (args.source, args.payload, args.deps_root, args.cache):
        if path is not None and not path.is_absolute(): parser.error("All supplied paths must be absolute")
    if args.out.resolve().is_relative_to(args.payload.resolve() if args.role == "consume" else args.source.resolve()):
        parser.error("Output must be separate from source/payload")
    args.out.mkdir(parents=True)
    records = []
    try:
        identity = native_tuple(args.out, records, args.hosted)
        if args.role in ("probe", "profiles"):
            actual, dirty = source_identity(args, records)
            if args.role == "profiles":
                command = [sys.executable, "-B", args.source / "test/runtime/profile_gate.py", args.profile,
                           "--source", args.source, "--deps-root", args.out, "--compiler", os.environ.get("CXX", "cl.exe" if identity["system"] == "Windows" else "c++"),
                           "--cmake", "cmake", "--ctest", "ctest", "--jobs", str(args.jobs)]
                env = dict(os.environ, ENGINE_SIM_GATE_OUTPUT=str(args.out))
                if args.cache:
                    env["ENGINE_SIM_PROFILE_CACHE"] = str(args.cache)
                if not args.offline:
                    command.append("--prepare-converter-cache")
                execute(command, args.source, args.out, records, env)
            result = {"status": "pass", "role": args.role, "profile": args.profile, "revision": actual, "dirty": dirty, "native": identity}
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
