"""Build and run the synthesis consumer in a source tree containing only DSP."""
import argparse
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

SOURCES = (
    "convolution_filter", "derivative_filter", "filter", "jitter_filter", "leveling_filter",
    "low_pass_filter", "synthesizer",
)
HEADERS = (*SOURCES, "acoustic_excitation", "butterworth_low_pass_filter", "constants",
           "random_stream", "ring_buffer", "simulation_fault", "utilities")
CONSUMER = ("synthesis_consumer.cpp", "synthesis_scenario.hpp", "synthesis_trace.hpp")


def verify(args, work, evidence):
    root = work / "source"
    build = work / "build"
    root.mkdir(parents=True)
    paths = ["modules/synthesis/CMakeLists.txt",
             *(f"src/{name}.cpp" for name in SOURCES),
             *(f"include/{name}.h" for name in HEADERS),
             *(f"test/runtime/{name}" for name in CONSUMER)]
    for name in paths:
        target = root / name
        target.parent.mkdir(exist_ok=True, parents=True)
        shutil.copyfile(args.source / name, target)
    (root / "CMakeLists.txt").write_text(
        """cmake_minimum_required(VERSION 3.25)
project(synthesis_consumer LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()
add_subdirectory(modules/synthesis)
add_executable(synthesis-consumer test/runtime/synthesis_consumer.cpp)
target_link_libraries(synthesis-consumer PRIVATE engine-sim-synthesis)
add_test(NAME SynthesisConsumer COMMAND synthesis-consumer ${SYNTHESIS_EVIDENCE})
""", encoding="utf-8")
    commands = []
    configure = [args.cmake, "-S", root, "-B", build, "-G", args.generator,
                 "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                 "-DCMAKE_CXX_COMPILER=" + args.compiler]
    if args.platform:
        configure.extend(["-A", args.platform])
    if evidence:
        configure.append(f"-DSYNTHESIS_EVIDENCE={evidence}")

    for argv in (configure, [args.cmake, "--build", build, "--config", "Release", "--verbose"],
                 [args.ctest, "--test-dir", build, "-C", "Release", "--no-tests=error", "--verbose"]):
        argv = list(map(str, argv))
        executed = subprocess.run(argv, cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log = work / f"{len(commands) + 1:02}.log"
        log.write_bytes(executed.stdout)
        commands.append(dict(argv=argv, cwd=str(work), exit=executed.returncode, log=log.name))
        (work / "commands.json").write_text(json.dumps(commands, indent=2) + "\n", encoding="utf-8")
        print(executed.stdout.decode(errors="replace"), end="")
        executed.check_returncode()
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--platform", default="")
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--ctest", required=True)
    args = parser.parse_args()
    evidence = os.environ.get("ENGINE_SIM_GATE_OUTPUT")
    if evidence:
        evidence = Path(evidence).resolve(strict=True)
        verify(args, evidence / "synthesis-projection", evidence)
    else:
        with tempfile.TemporaryDirectory(prefix="engine-sim-synthesis-") as scratch:
            verify(args, Path(scratch), None)
