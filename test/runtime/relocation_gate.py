"""Install the exact code SDK, relocate it, and exercise its negative boundaries."""
import argparse
import os
from pathlib import Path
import shutil
import sys
import tempfile

from installed_consumer import file_sha256
from profile_gate import Audit, OPTIONAL_INPUT, SOLVER, read_json, write_json


DATA = Path("share/engine-sim-runtime")
ARCHIVES = ("engine-sim-runtime-json-private", "engine-sim-runtime-backend", "engine-sim-assembly",
            "engine-sim-json", "engine-sim-config", "engine-sim", SOLVER)
NOTICE_INPUTS = {
    "EngineSim-MIT.txt": "LICENSE",
    "THIRD_PARTY_NOTICES.md": "cmake/runtime-sdk-notices.md",
    "nlohmann-json-MIT.txt": "modules/json/third_party/nlohmann/LICENSE.MIT",
    "Solver-MIT.txt": f"dependencies/submodules/{SOLVER}/LICENSE",
}


def check_payload(source, build, prefix, variant):
    names = {
        Path("include/engine_sim/runtime.h"): source / "include/engine_sim/runtime.h",
        **{DATA / "schema" / name: source / "schemas" / name
           for name in ("engine-config-v1.schema.json", "engine-config-v2.schema.json")},
        **{DATA / "licenses" / name: source / origin for name, origin in NOTICE_INPUTS.items()},
        **{Path(f"lib/cmake/EngineSimRuntime/{name}"): origin for name, origin in (
            ("EngineSimRuntimeConfig.cmake", build / "sdk-config/Release/EngineSimRuntimeConfig.cmake"),
            ("EngineSimRuntimeConfigVersion.cmake", build / "EngineSimRuntimeConfigVersion.cmake"))},
    }
    if variant == "static":
        runtime = Path("lib/libengine-sim-runtime.a")
        binaries = {runtime, *(Path(f"lib/engine-sim-runtime/lib{name}.a") for name in ARCHIVES)}
        links = set()
    else:
        runtime = Path("lib/libengine-sim-runtime.so.2.0.0")
        binaries = {runtime}
        links = {Path("lib/libengine-sim-runtime.so"), Path("lib/libengine-sim-runtime.so.2")}
    actual = {p.relative_to(prefix) for p in prefix.rglob("*") if p.is_file() or p.is_symlink()}
    assert actual == set(names) | binaries | links, sorted(map(str, actual ^ (set(names) | binaries | links)))
    for path, original in names.items():
        installed = prefix / path
        assert not installed.is_symlink() and file_sha256(installed) == file_sha256(original), path
    for path in links:
        link = prefix / path
        assert link.is_symlink() and not link.readlink().is_absolute() and link.resolve() == prefix / runtime
    assert all(not (prefix / path).is_symlink() for path in binaries)
    for name in ("consumer-build/runtime-c-consumer.map", "consumer-build/runtime-cxx-consumer.map"):
        text = (prefix.parent / name).read_text(errors="replace")
        assert "libengine-sim-runtime" in text and not OPTIONAL_INPUT.search(text)
    commands = [entry["command"] for entry in read_json(prefix.parent / "consumer-build/compile_commands.json")]
    assert commands and all("/work/SDK espaço Ω/include" in row and not OPTIONAL_INPUT.search(row) for row in commands)
    write_json(build / "code-only-files.json", {p.as_posix(): file_sha256(prefix / p) for p in sorted(actual)})
    return runtime


def rejected_install(audit, build, artifact, label, *, rewrite):
    original = artifact.read_bytes() if rewrite else None
    saved = artifact.with_name(artifact.name + ".saved")
    destination = audit.root / ("refused-" + label)
    if rewrite:
        artifact.write_bytes(original + b"\n")
    else:
        artifact.rename(saved)
    expected = "SDK input changed since configure" if rewrite else "Missing built SDK artifact"
    try:
        message = audit.run("reject-" + label, [audit.args.cmake, "--install", build, "--config", "Release",
                             "--prefix", destination, "--component", "RuntimeSDK"], expected=expected)
        assert artifact.name in message and not any(destination.rglob("*"))
    finally:
        if rewrite:
            artifact.write_bytes(original)
        else:
            saved.rename(artifact)


def rejected_consumers(audit, prefix, runtime, variant):
    library = prefix / (Path(f"lib/engine-sim-runtime/lib{SOLVER}.a") if variant == "static" else runtime)
    external = audit.root / "outside-prefix-library"
    library.rename(external)
    project = audit.args.source / "examples/runtime/consumer"
    command = [audit.args.cmake, "-S", project, "-G", "Ninja",
               f"-DCMAKE_PREFIX_PATH={prefix}", "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
               "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF"]
    try:
        audit.run("missing-installed-library", [*command, "-B", audit.root / "missing-consumer"],
                  expected="Missing or invalid EngineSim Runtime SDK file")
        library.symlink_to(external)
        audit.run("escaped-installed-library", [*command, "-B", audit.root / "escaping-consumer"],
                  expected="EngineSim Runtime SDK file escapes installed prefix")
    finally:
        if library.is_symlink():
            library.unlink()
        external.rename(library)


def opt_in_example(audit, source, variant):
    fixture = source / "examples/runtime/fixture"
    fixture.mkdir(parents=True)
    manifest = source / "cmake/runtime-example.json"
    original_manifest = audit.args.source / "cmake/runtime-example.json"
    shutil.copyfile(original_manifest, manifest)
    names = {row["path"]: row["sha256"] for row in read_json(manifest)["files"]}
    for name, digest in names.items():
        original = audit.args.source / "examples/runtime/fixture" / name
        assert file_sha256(original) == digest
        shutil.copyfile(original, fixture / name)
    options = ["-DENGINE_SIM_INSTALL_RUNTIME_EXAMPLE=ON"]
    invalid = read_json(manifest)
    invalid["files"].append({"path": "engine.mr", "sha256": names["main.json"]})
    write_json(manifest, invalid)
    try:
        audit.run("reject-legacy-example", [audit.args.cmake, "--preset", "runtime-" + variant,
                  "-G", "Ninja", *options], cwd=source, env=dict(audit.env, ES_OUT=str(audit.root / "bad-example")),
                  expected="Unexpected or duplicate SDK example file: engine.mr")
    finally:
        shutil.copyfile(original_manifest, manifest)
    out = audit.root / "optional"
    build = audit.configure(source, out, "runtime-" + variant, *options)
    audit.build(build, "engine-sim-runtime")
    prefix = out / "prefix"
    audit.run("install-optional-example", [audit.args.cmake, "--install", build, "--config", "Release",
              "--prefix", prefix, "--component", "RuntimeSDK"])
    example = prefix / DATA / "example"
    assert {item.name: file_sha256(item) for item in example.iterdir()} == names
    baseline = read_json(source.parent / "out" / ("runtime-" + variant) / "code-only-files.json")
    without_example = {file.relative_to(prefix).as_posix() for file in prefix.rglob("*")
                       if file.is_file() and not file.is_relative_to(example)}
    assert without_example == set(baseline)


def verify(audit, evidence):
    args = audit.args
    variant = args.variant
    source = audit.sdk_source()
    out = source.parent / "out"
    receipt = audit.bootstrap(source, out, "runtime")
    assert set(receipt["git"]) == {SOLVER} and not receipt["archives"]
    isolated = audit.sdk(source, out, variant)
    build = out / ("runtime-" + variant)
    prefix = build / "installed-consumer/SDK espaço Ω"
    library = check_payload(source, build, prefix, variant)
    rejected_install(audit, build, source / "schemas/engine-config-v1.schema.json", "changed-schema", rewrite=True)
    rejected_install(audit, build, build / "sdk-config/Release/EngineSimRuntimeConfig.cmake", "changed-config", rewrite=True)
    rejected_install(audit, build, build / library.name, "missing-binary", rewrite=False)
    rejected_consumers(audit, prefix, library, variant)
    opt_in_example(audit, source, variant)
    report = {"code_only": isolated, "mixed": None}

    cache = os.environ.get("ENGINE_SIM_PROFILE_CACHE")
    if evidence or cache:
        mixed = evidence / "mixed-inputs" if evidence else audit.root / "mixed"
        if not (mixed / "dependency-receipt.json").is_file():
            audit.bootstrap(args.source, mixed, "runtime",
                            ("--with-desktop", "--with-capture", "--with-converter", "--tests"),
                            Path(cache) if cache else args.deps_root / "cache")
        receipt = read_json(mixed / "dependency-receipt.json")
        assert receipt["lock_sha256"] == file_sha256(args.source / "cmake/dependencies.lock.json")
        assert set(receipt["archives"]) == {"sdl3", "googletest", "boost"}
        assert set(receipt["git"]) == {SOLVER, "piranha"}
        combined = audit.sdk(args.source, mixed, variant, mixed=True)
        mixed_build = mixed / ("runtime-" + variant)
        check_payload(args.source, mixed_build, mixed_build / "installed-consumer/SDK espaço Ω", variant)
        assert combined == isolated, [key for key in isolated if combined[key] != isolated[key]]
        report["mixed"] = combined
    write_json(audit.root / "result.json", report)
    print(f"Verified relocated {variant} RuntimeSDK")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", choices=("static", "shared"), required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--deps-root", type=Path, required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--ctest", required=True)
    parser.add_argument("--jobs", default=os.environ.get("ENGINE_SIM_PROFILE_JOBS", "3"))
    args = parser.parse_args()
    if output := os.environ.get("ENGINE_SIM_GATE_OUTPUT"):
        evidence = Path(output) / "relocation"
        root = evidence / args.variant
        root.mkdir(parents=True)
        verify(Audit(args, root), evidence)
    else:
        with tempfile.TemporaryDirectory(prefix=f"relocated-{args.variant}-", dir=args.deps_root) as temporary:
            verify(Audit(args, Path(temporary)), None)


if __name__ == "__main__":
    main()
