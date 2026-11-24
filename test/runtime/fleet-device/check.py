"""Exercise the device-free adapter and the explicitly selected physical path."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile


def command(argv, log, *, env=None, expected=None):
    args = [str(value) for value in argv]
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            errors="replace", env=env, timeout=180)
    with log.open("a", encoding="utf-8") as stream:
        stream.write(f"$ {shlex.join(args)}\n{result.stdout}exit {result.returncode}\n")
    if expected:
        if result.returncode == 0 or expected not in result.stdout:
            raise ValueError(f"missing negative setup assertion: {expected}")
    elif result.returncode != 0:
        print(result.stdout, end="")
        raise ValueError(f"step failed: {args[0]}")
    return result.stdout


def install(build, prefix, config, component, log):
    return command(["cmake", "--install", build, "--prefix", prefix, "--config", config, "--component", component], log)


def device_free(args, work, log):
    print(command([args.kernel], log), end="")
    prefix = work / "code-only"
    install(args.build, prefix, args.config, "FleetSDK", log)
    paths = [p.relative_to(prefix).as_posix() for p in prefix.rglob("*") if p.is_file()]
    if not paths or any(re.search(r"portaudio|device|/example/|\.(wav|json)$", name, re.I) for name in paths):
        raise ValueError("code-only SDK contains device or example dependencies")
    example = work / "standalone"
    shutil.copytree(args.source / "examples/fleet", example)
    common = [f"-DCMAKE_PREFIX_PATH={prefix}", "-DCMAKE_BUILD_TYPE=Release",
              "-DCMAKE_DISABLE_FIND_PACKAGE_portaudio=ON", "-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON"]
    command(["cmake", "-S", example, "-B", work / "off", *common, "-DENGINE_SIM_FLEET_PORTAUDIO=OFF"], log)
    command(["cmake", "--build", work / "off", "--config", "Release"], log)
    if list((work / "off").rglob("*fleet-live*")):
        raise ValueError("default OFF example built a live host")
    command(["cmake", "-S", example, "-B", work / "no-provider", *common, "-DENGINE_SIM_FLEET_PORTAUDIO=ON"],
            log, expected="PkgConfig" if os.name != "nt" else "PortAudio v19 shared package")
    print("FleetSDK code-only and standalone OFF built without PortAudio or device discovery")


def physical(args, work, log):
    endpoint = os.environ.get("ES_FLEET_DEVICE")
    if endpoint is None or not endpoint.isdecimal():
        raise ValueError("Select the physical PortAudio output index with ES_FLEET_DEVICE")
    producer = work / "producer"
    prefix = work / "deployed"
    command(["cmake", "-S", args.source, "-B", producer, "-DCMAKE_BUILD_TYPE=Release",
             "-DBUILD_TESTING=OFF", "-DENGINE_SIM_BUILD_RUNTIME=ON", "-DENGINE_SIM_RUNTIME_SHARED=ON",
             "-DENGINE_SIM_BUILD_DESKTOP=OFF", "-DENGINE_SIM_INSTALL_FLEET_EXAMPLE=ON"], log)
    command(["cmake", "--build", producer, "--config", "Release", "--target", "engine-sim-fleet", "--parallel", "6"], log)
    for component in ("FleetSDK", "FleetSDKExamples"):
        install(producer, prefix, "Release", component, log)
    examples = next(prefix.rglob("fixture-single")).parent
    build = work / "live"
    command(["cmake", "-S", examples, "-B", build, f"-DCMAKE_PREFIX_PATH={prefix}",
             "-DCMAKE_BUILD_TYPE=Release", "-DENGINE_SIM_FLEET_PORTAUDIO=ON"], log)
    command(["cmake", "--build", build, "--config", "Release", "--target", "engine-sim-fleet-live"], log)
    suffix = ".exe" if os.name == "nt" else ""
    host = next(build.rglob("engine-sim-fleet-live" + suffix))
    env = os.environ.copy()
    if os.name == "nt":
        env["PATH"] = os.pathsep.join([str(prefix / "bin"), env.get("PATH", "")])
    first, second = (examples / f"fixture-{kind}" for kind in ("single", "heavy"))
    observed = command([host, endpoint, "48000", "256", first / "main.json", first,
                        second / "main.json", second], log, env=env)
    print(observed, end="")
    location = re.search(r"^portaudio_library=(.+)$", observed, re.M)
    if not location:
        raise ValueError("missing active PortAudio library identity")
    library = Path(location.group(1)).resolve(strict=True)
    print(f"PortAudio SHA-256 {hashlib.sha256(library.read_bytes()).hexdigest()}")
    print("The selected endpoint ran the installed live host. This captures backend timing estimates.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--physical", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="fleet-device-") as folder:
        work = Path(folder)
        log = work / "device-transcript.txt"
        try:
            physical(args, work, log) if args.physical else device_free(args, work, log)
        finally:
            if "ES_FLEET_EVIDENCE" in os.environ and log.is_file():
                shutil.copy2(log, Path(os.environ["ES_FLEET_EVIDENCE"]) / log.name)
            elif log.is_file():
                print(log.read_text(), end="")
