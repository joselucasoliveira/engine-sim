"""Run independent cold processes for each operation and retain unfiltered observations."""
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile


CONTROLS = {
    "safe": (),
    "malloc": ("malloc",),
    "calloc": ("calloc",),
    "realloc": ("realloc",),
    "free": ("free",),
    "aligned_alloc": ("aligned_alloc",),
    "aligned_free": ("free",),
    "posix_memalign": ("posix_memalign",),
    "new": ("_Znwm",),
    "array_new": ("_Znam",),
    "nothrow_new": ("_ZnwmRKSt9nothrow_t",),
    "nothrow_array_new": ("_ZnamRKSt9nothrow_t",),
    "aligned_new": ("_ZnwmSt11align_val_t",),
    "aligned_array_new": ("_ZnamSt11align_val_t",),
    "nothrow_aligned_new": ("_ZnwmSt11align_val_tRKSt9nothrow_t",),
    "nothrow_aligned_array_new": ("_ZnamSt11align_val_tRKSt9nothrow_t",),
    "delete": ("_ZdlPv",),
    "array_delete": ("_ZdaPv",),
    "sized_delete": ("_ZdlPvm",),
    "sized_array_delete": ("_ZdaPvm",),
    "aligned_delete": ("_ZdlPvSt11align_val_t",),
    "aligned_array_delete": ("_ZdaPvSt11align_val_t",),
    "sized_aligned_delete": ("_ZdlPvmSt11align_val_t",),
    "sized_aligned_array_delete": ("_ZdaPvmSt11align_val_t",),
    "nested_allocation": ("malloc", "free"),
    "exception": ("__cxa_allocate_exception", "__cxa_throw"),
    "lazy_init": ("__cxa_guard_acquire",),
    "mutex_lock": ("pthread_mutex_lock",),
    "recursive_lock": ("pthread_mutex_lock",),
    "mutex_try": ("pthread_mutex_trylock",),
    "read_lock": ("pthread_rwlock_rdlock",),
    "write_lock": ("pthread_rwlock_wrlock",),
    "read_try": ("pthread_rwlock_tryrdlock",),
    "write_try": ("pthread_rwlock_trywrlock",),
    "spin_lock": ("pthread_spin_lock",),
    "spin_try": ("pthread_spin_trylock",),
    "condition_wait": ("pthread_cond_wait",),
    "condition_timed_wait": ("pthread_cond_timedwait",),
    "sleep": ("nanosleep",),
    "yield": ("sched_yield",),
    "join": ("pthread_join",),
    "thread_create": ("pthread_create",),
    "file_open": ("fopen",),
    "format": ("snprintf",),
}
HIT = re.compile(r"^observed (\S+) ([1-9]\d*) bytes=\d+ caller=0x[0-9a-f]+ module=(.+) offset=0x[0-9a-f]+$")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(out, label, command, check=True):
    log = out / (label + ".log")
    command = list(map(str, command))
    with log.open("x") as file:
        file.write("$ " + shlex.join(command) + "\n")
        file.flush()
        try:
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, errors="replace", timeout=15)
        except subprocess.TimeoutExpired as exc:
            if exc.output:
                file.write(exc.output.decode(errors="replace"))
            file.write("\nTIMEOUT\n")
            raise
        file.write(result.stdout)
        file.write("exit " + str(result.returncode) + "\n")
    require(not check or result.returncode == 0, f"{label} exited {result.returncode}. Evidence: {log}")
    return result


def identity(out, readelf, objdump, binaries, disassemble=False):
    with (out / "sha256.txt").open("x") as hashes:
        for binary in binaries:
            hashes.write(f"{hashlib.sha256(binary.read_bytes()).hexdigest()}  {binary}\n")
            run(out, binary.name + "-elf", [readelf, "-lW", "-dW", "-Ws", binary])
            run(out, binary.name + "-dependencies", ["ldd", binary])
            if disassemble:
                run(out, binary.name + "-disassembly", [objdump, "-d", "-C", binary])


def feature_controls(out, label, compiler, language, standard):
    source = Path(__file__).parent / "feature.c"
    args = [compiler, "-x", language, "-std=" + standard, "-O1", source]
    run(out, label + "-version", [compiler, "--version"])
    static = []
    dynamic = []
    for case in ("safe", "adverse"):
        flags = [] if case == "safe" else ["-DPROBE_ADVERSE"]
        effects_obj = out / (label + "-effects-" + case + ".o")
        static.append(run(out, effects_obj.stem, args + flags
                          + ["-Werror=function-effects", "-Werror=attributes", "-c", "-o", effects_obj], check=False))
        dynamic_bin = out / (label + "-rtsan-" + case)
        built = run(out, dynamic_bin.name + "-build", args + flags
                    + ["-fsanitize=realtime", "-Werror=attributes", "-o", dynamic_bin], check=False)
        dynamic.append(run(out, dynamic_bin.name, [dynamic_bin], check=False) if built.returncode == 0 else None)
    # A clean build alone does not prove that the compiler diagnosed an effect.
    effect_pass = static[0].returncode == 0 and static[1].returncode != 0 and bool(re.search(
        r"error:.*(?:malloc|free).*nonblocking|error:.*nonblocking.*(?:malloc|free)", static[1].stdout))
    rtsan_pass = (all(dynamic) and dynamic[0].returncode == 0 and dynamic[1].returncode != 0
                  and "RealtimeSanitizer" in dynamic[1].stdout and "malloc" in dynamic[1].stdout)
    return {"effect_check": bool(effect_pass), "rtsan": bool(rtsan_pass),
            "effect_compile_exit": [case.returncode for case in static],
            "rtsan_sentinel_exit": [case.returncode if case is not None else None for case in dynamic]}


def features(out, label, compiler, language, standard):
    result = feature_controls(out, label, compiler, language, standard)
    return f"{label} effect-check={result['effect_check']} rtsan={result['rtsan']}"


def record_features(out, cc, cxx):
    versions = {label: feature_controls(out, label, compiler, language, standard)
                for label, compiler, language, standard in (
                    ("c11", cc, "c", "c11"), ("cxx17", cxx, "c++", "c++17"))}
    result = {**versions, "scope": "compiler capability and matched adverse controls",
              "installed_boundary": "pending an independently observed installed ABI boundary"}
    (out / "features.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


def coverage(out, static, shared, shared_kernel, nested, cc, cxx, readelf, objdump):
    static, shared, shared_kernel, nested = map(Path, (static, shared, shared_kernel, nested))
    observer = next(shared_kernel.parent.glob("libengine-sim-r3-observer.so"))
    identity(out, readelf, objdump, (static, shared, shared_kernel, nested, observer), disassemble=True)
    for linkage, binary in (("static", static), ("shared", shared)):
        for case, expected in CONTROLS.items():
            result = run(out, f"{linkage}-{case}", [binary, case])
            lines = result.stdout.splitlines()
            require(len(lines) > 0 and re.fullmatch(r"owner \d+ control " + case, lines[0]),
                    f"{linkage}/{case} did not reach a fresh owner")
            hits = [HIT.fullmatch(line) for line in lines[1:]]
            require(all(hits), f"{linkage}/{case} malformed observer results")
            require(tuple(hit[1] for hit in hits) == expected, f"{linkage}/{case} expected {expected}, got {lines[1:]}")
            caller = static if linkage == "static" else (nested if case == "nested_allocation" else shared_kernel)
            require(all(Path(hit[3]).resolve() == caller.resolve() for hit in hits),
                    f"{linkage}/{case} violation from an unexpected caller: {lines[1:]}")
            if case != "condition_wait":
                require(all(hit[2] == "1" for hit in hits), f"{linkage}/{case} duplicate call attribution")
    summary = [
        f"{len(CONTROLS)} separate children per linkage, including copy/zero and a transitive DSO allocation/free",
        features(out, "configured-c11", cc, "c", "c11"),
        features(out, "configured-cxx17", cxx, "c++", "c++17"),
    ]
    if "clang" not in Path(cxx).name:
        for compiler, language, standard in (("clang", "c", "c11"), ("clang++", "c++", "c++17")):
            available = shutil.which(compiler)
            if available:
                summary.append(features(out, f"alternate-{standard}", available, language, standard))
    summary.append("Exception allocation/throw entry observed and stack unwinding witnessed by cleanup. "
                   "Libstdc++ internal unwinder, libc internal allocation, immune/hidden symbols and non-ELF "
                   "native linkages need independent diagnostics.")
    (out / "summary.txt").write_text("\n".join(summary) + "\n")
    print(*summary, sep="\n")


def raw(out, observed, plain, source):
    observed, plain, source = map(Path, (observed, plain, source))
    fixture = source / "examples/runtime/fixture"
    kohler = source / "assets/engines/atg-video-1/02_kohler_ch750.json"
    impulse = source / "es/sound-library/smooth/smooth_39.wav"
    models = (fixture / "main.json", fixture / "impulse.wav", kohler, impulse)
    with (out / "sha256.txt").open("x") as file:
        for path in (observed, plain, *models):
            file.write(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path}\n")

    def payload(run):
        return [line for line in run.stdout.splitlines()
                if line.startswith(("snapshot ", "pcm ", "startup ", "first_tick "))]

    for label, args in [
        *((f"{mode}-{rate}", [mode, rate]) for mode in ("free", "driven") for rate in (44100, 48000)),
        ("saturation", ["fault"]),
    ]:
        instrumented = run(out, label + "-observed", [observed, source, *args])
        control = run(out, label + "-plain", [plain, source, *args])
        actual, reference = payload(instrumented), payload(control)
        require(actual and actual == reference, f"{label} observer changed PCM/telemetry: {out}")
        digest = hashlib.sha256("\n".join(actual).encode()).hexdigest()
        (out / (label + ".txt")).write_text(f"{digest}  exact PCM, counts and telemetry against plain executable\n")
        print(label, digest)


@contextlib.contextmanager
def output(mode):
    external = os.environ.get("ES_FLEET_EVIDENCE")
    if external:
        directory = Path(external) / mode
        directory.mkdir()
        yield directory
    else:
        with tempfile.TemporaryDirectory(prefix=f"engine-sim-r3-{mode}-") as directory:
            yield Path(directory)


if __name__ == "__main__":
    kind, *arguments = sys.argv[1:]
    with output(kind) as destination:
        {"coverage": coverage, "raw": raw, "features": record_features}[kind](destination, *arguments)
