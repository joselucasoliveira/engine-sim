"""Build a programmatic physics consumer with only Config and the locked solver."""
import argparse
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from synthesis_projection import verify as verify_synthesis

SOURCES = (
    "camshaft", "crankshaft", "combustion_chamber", "connecting_rod", "cylinder_bank", "cylinder_head",
    "delay_filter", "direct_throttle_linkage", "dynamometer", "engine", "exhaust_system", "fuel",
    "function", "gas_system", "gaussian_filter", "governor", "ignition_module", "impulse_response", "intake", "part",
    "piston", "piston_engine_simulator", "simulator", "standard_valvetrain", "starter_motor", "throttle",
    "transmission", "utilities", "valvetrain", "vehicle", "vehicle_drag_constraint", "vtec_valvetrain",
)
HEADERS = (*SOURCES, "acoustic_excitation", "application_settings", "checked_solver", "constants",
           "engine_definition", "filter", "random_stream", "ring_buffer", "scs", "simulation_fault", "units")
DSP_HEADERS = ("synthesizer.h", "sample_clock.h", "engine_session.h", "desktop_engine_views.h", "nlohmann/json.hpp")
SOLVER = Path("dependencies/submodules/simple-2d-constraint-solver")

def verify_physics(args, work, trace, deny):
    source = work / "source"
    build = work / "build"
    source.mkdir(parents=True)
    files = (
        "modules/physics/CMakeLists.txt", "modules/assembly/src/mechanical.cpp", "modules/assembly/src/mechanical.hpp",
        "modules/assembly/src/definition.cpp", "modules/assembly/src/definition.hpp",
        "dependencies/submodules/CMakeLists.txt", "cmake/LockedSources.cmake", "cmake/SolverCompatibility.h",
        "cmake/dependencies.lock.json", "test/runtime/physics_consumer.cpp", "test/runtime/physics_scenario.hpp",
        "examples/runtime/programmatic/model.hpp",
        *(f"src/{name}.cpp" for name in SOURCES), *(f"include/{name}.h" for name in HEADERS),
    )
    for name in files:
        destination = source / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(args.source / name, destination)
    shutil.copytree(args.source / "modules/config", source / "modules/config")
    shutil.copytree(args.source / SOLVER, source / SOLVER, ignore=shutil.ignore_patterns(".git"))
    gitdir = subprocess.check_output(
        ["git", "-C", str(args.source / SOLVER), "rev-parse", "--absolute-git-dir"], text=True).strip()
    (source / SOLVER / ".git").write_text(f"gitdir: {gitdir}\n", encoding="utf-8")

    project = """cmake_minimum_required(VERSION 3.25)
project(physics_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()

# Select the locked solver branch in the existing dependency definition.
set(ENGINE_SIM_BUILD_RUNTIME ON)
set(PIRANHA_ENABLED OFF)
add_subdirectory(dependencies/submodules)
add_subdirectory(modules/config)
add_subdirectory(modules/physics)
add_executable(physics-consumer test/runtime/physics_consumer.cpp)
target_link_libraries(physics-consumer PRIVATE engine-sim-physics)
add_test(NAME PhysicsConsumer COMMAND physics-consumer ${PHYSICS_TRACE})
"""
    for index, header in enumerate(deny):
        (source / f"forbidden-{index}.cpp").write_text(f'#include "{header}"\n', encoding="utf-8")
        project += (f"add_executable(forbidden-{index} EXCLUDE_FROM_ALL forbidden-{index}.cpp)\n"
                    f"target_link_libraries(forbidden-{index} PRIVATE engine-sim-physics)\n")
    (source / "CMakeLists.txt").write_text(project, encoding="utf-8")

    commands = []
    def invoke(argv, missing=None):
        argv = list(map(str, argv))
        completed = subprocess.run(argv, cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log = work / f"{len(commands) + 1:02}.log"
        output = completed.stdout.decode(errors="replace")
        log.write_bytes(completed.stdout)
        commands.append(dict(argv=argv, cwd=str(work), exit=completed.returncode, log=log.name,
                             required_missing_header=missing))
        (work / "commands.json").write_text(json.dumps(commands, indent=2) + "\n", encoding="utf-8")
        print(output, end="")
        if missing is None:
            completed.check_returncode()
        elif completed.returncode == 0 or "fatal error" not in output.lower() or missing.lower() not in output.lower():
            raise RuntimeError(f"Unexpected compile result for forbidden include {missing}")

    configure = [args.cmake, "-S", source, "-B", build, "-G", args.generator,
                 "-DCMAKE_CXX_COMPILER=" + args.compiler, "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
    if args.platform:
        configure.extend(["-A", args.platform])
    if trace is not None:
        configure.append(f"-DPHYSICS_TRACE={trace}")
    invoke(configure)
    invoke([args.cmake, "--build", build, "--config", "Release", "--parallel", "3", "--verbose"])
    invoke([args.ctest, "--test-dir", build, "-C", "Release", "--no-tests=error", "--verbose"])
    for index, header in enumerate(deny):
        invoke([args.cmake, "--build", build, "--config", "Release", "--target", f"forbidden-{index}"], missing=header)


def verify(args, work, evidence):
    trace = (evidence / "physics-ticks.le64") if evidence and not args.direction else None
    verify_physics(args, work / "physics", trace, DSP_HEADERS if args.direction else ())
    if args.direction:
        verify_synthesis(args, work / "synthesis", None, deny=("simulator.h", "scs.h", "engine_sim/config-v1.hpp"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--platform", default="")
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--ctest", required=True)
    parser.add_argument("--direction", action="store_true")
    args = parser.parse_args()
    target = os.environ.get("ENGINE_SIM_GATE_OUTPUT")
    if target:
        evidence = Path(target).resolve(strict=True)
        verify(args, evidence / ("module-direction" if args.direction else "physics-projection"), evidence)
    else:
        with tempfile.TemporaryDirectory(prefix="engine-sim-physics-") as scratch:
            verify(args, Path(scratch), None)
