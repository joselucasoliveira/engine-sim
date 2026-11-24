"""Execute the published Fleet quickstart against static and shared installs."""

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import sys
import tempfile

import fleet_sdk


def shell_block(guide, title):
    section = guide.split(f"\n## {title}\n", 1)[1]
    return "set -eu\n" + section.split("```sh\n", 1)[1].split("\n```", 1)[0] + "\n"


def quickstart(args, work):
    guide = (args.source / "docs/fleet-guide.md").read_text(encoding="utf-8")
    install = work / "install-guide.sh"
    install.write_text(shell_block(guide, "Install the SDK and examples"), encoding="utf-8")
    examples = json.loads((args.source / "cmake/fleet-example.json").read_text(encoding="utf-8"))["files"]
    expected = {item["path"]: item["sha256"] for item in examples}
    shell = shutil.which("bash")
    fleet_sdk.need(shell, "The quickstart requires Bash")
    for linkage in ("static", "shared"):
        local = work / linkage
        local.mkdir()
        build = local / "producer"
        build.mkdir()
        shutil.copyfile(args.deps_file, build / "deps.cmake")
        original = local / "original prefix"
        environment = dict(os.environ, ES_OUT=str(build), FLEET_LINKAGE=linkage, FLEET_PREFIX=str(original),
                           CMAKE_GENERATOR="Ninja", CMAKE_BUILD_PARALLEL_LEVEL="8")
        fleet_sdk.run([shell, install], args.source, local / "install.log", env=environment)

        transfer = local / "transfer"
        transfer.mkdir()
        moved = transfer / "SDK espaço Ω"
        original.rename(moved)
        fleet_sdk.need(fleet_sdk.inventory(moved / "share/engine-sim-fleet/example") == expected,
                       "Installed quickstart files differ from the allowlist")
        required = {"fleet.h", "EngineSimFleetConfig.cmake", "EngineSimFleetConfigVersion.cmake"} | fleet_sdk.LICENSES
        fleet_sdk.need(required <= {path.name for path in moved.rglob("*") if path.is_file()},
                       "Installed quickstart is missing ABI files or SDK notices")
        if platform.system() == "Linux" and linkage == "shared":
            library = next(moved.rglob("libengine-sim-fleet.so"))
            fleet_sdk.symbols(library, fleet_sdk.FLEET_EXPORTS, local, local / "symbols.log")
        document = transfer / "consumer-guide.sh"
        document.write_text(shell_block(guide, "Run the installed hosts"), encoding="utf-8")
        if platform.system() == "Linux":
            ninja = shutil.which("ninja")
            fleet_sdk.need(ninja, "The quickstart requires the installed Ninja build tool")
            launcher = ["bwrap", "--unshare-all", "--die-with-parent", "--new-session"]
            for directory in ("/usr", "/lib", "/lib64", "/bin", "/etc"):
                if Path(directory).exists():
                    launcher += ["--ro-bind", directory, directory]
            launcher += ["--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp", "--bind", transfer, "/work",
                         "--dir", "/tools", "--ro-bind", Path(ninja).resolve(), "/tools/ninja",
                         "--chdir", "/work", "--clearenv", "--setenv", "PATH", "/tools:/usr/bin:/bin",
                         "--setenv", "LC_ALL", "C.UTF-8", "--setenv", "FLEET_PREFIX", "/work/SDK espaço Ω",
                         "--setenv", "FLEET_BUILD", "/work/consumer",
                         "--setenv", "PRODUCER_CHECKOUT", args.source, "--", "/usr/bin/bash", "-ec",
                         'test ! -e "$PRODUCER_CHECKOUT/CMakeLists.txt"; exec bash /work/consumer-guide.sh']
            fleet_sdk.run(launcher, transfer, local / "consumer.log")
        else:
            environment = dict(os.environ, FLEET_PREFIX=str(moved), FLEET_BUILD=str(transfer / "consumer"))
            fleet_sdk.run([shell, document], transfer, local / "consumer.log", env=environment)
        (local / "installed-files.json").write_text(
            json.dumps(fleet_sdk.inventory(moved), indent=2) + "\n", encoding="utf-8")
        print(f"Installed Fleet guide passed ({linkage})", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--deps-file", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if external := os.environ.get("ES_FLEET_EVIDENCE"):
        folder = Path(external) / "installed-fleet-guide"
        folder.mkdir()
        quickstart(args, folder)
    else:
        args.out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=args.out, prefix="fleet-guide-") as folder:
            quickstart(args, Path(folder))


if __name__ == "__main__":
    try:
        main()
    except RuntimeError as error:
        sys.exit(str(error))
