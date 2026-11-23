"""Run the public SDK and migration walkthroughs against an external fresh checkout."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from urllib.parse import unquote, urlsplit

from example_tests import wave
from programmatic_gate import matching_graph

SOURCE = Path(__file__).resolve().parents[2]
DOCUMENTS = ("README.md", "BUILDING.md", "TESTING.md", "docs/engine-config.md",
             "docs/engine-migration.md", "docs/legacy-converter.md", "docs/runtime-guide.md",
             "docs/engine-authoring.md", "docs/engine-authoring-catalog.md")
SOLVER = "simple-2d-constraint-solver"
GUIDE = "docs/runtime-guide.md"
MIGRATION = "docs/engine-migration.md"


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def save(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def json_output(traced_output):
    return json.loads(traced_output[traced_output.index("\n{") + 1:])


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_links():
    count = 0
    for path in map(SOURCE.joinpath, DOCUMENTS):
        text = path.read_text(encoding="utf-8")
        for destination in re.findall(r"\[[^\]\n]+\]\(([^)\n]+)\)", text):
            link = urlsplit(destination)
            if link.scheme or link.netloc:
                continue
            target = (path.parent / unquote(link.path)).resolve() if link.path else path
            assert target.is_relative_to(SOURCE) and target.exists(), (path, destination)
            if link.fragment and target.suffix == ".md":
                headings = re.findall(r"^#+ (.+)$", target.read_text(encoding="utf-8"), re.M)
                anchors = {re.sub(r"[^\w -]", "", line.lower()).replace(" ", "-") for line in headings}
                assert unquote(link.fragment) in anchors, (path, destination)
            count += 1
    return count


class Walkthrough:
    def __init__(self, work, evidence):
        self.work = work
        self.evidence = evidence
        self.checkout = work / "source"
        self.build = work / "output"
        self.env = {key: value for key, value in os.environ.items() if key not in
                    ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "CMAKE_PREFIX_PATH", "LIBRARY_PATH")}
        self.env.update(ES_OUT=str(self.build), CMAKE_GENERATOR="Ninja", LC_ALL="C.UTF-8", TZ="UTC",
                        GIT_ALLOW_PROTOCOL="file", GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        self.commands = []

    def run(self, name, *argv, cwd=None, expected=0, **environment):
        cwd = cwd or self.checkout
        argv = list(map(str, argv))
        result = subprocess.run(argv, cwd=cwd, env=dict(self.env, **{k: str(v) for k, v in environment.items()}),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=900)
        log = self.evidence / (name + ".log")
        log.write_bytes(result.stdout)
        self.commands.append({"argv": argv, "cwd": str(cwd), "exit": result.returncode, "log": log.name})
        save(self.evidence / "commands.json", self.commands)
        output = result.stdout.decode(errors="replace")
        if result.returncode != expected:
            raise RuntimeError(f"{name} exited {result.returncode}; expected {expected}. See {log}\n{output[-6000:]}")
        return output

    def checkout_source(self, names):
        self.run("fresh-checkout", "git", "clone", "--quiet", "--local", "--shared", SOURCE, self.checkout, cwd=self.work)
        for name in names:
            path = "dependencies/submodules/" + name
            pinned = self.run(name + "-pin", "git", "rev-parse", "HEAD:" + path).strip()
            current = self.run(name + "-origin", "git", "-C", SOURCE / path, "rev-parse", "HEAD").strip()
            assert pinned == current, (path, pinned, current)
            self.run(name + "-source", "git", "config", "submodule." + path + ".url", SOURCE / path)
        assert not self.run("clean-checkout", "git", "status", "--porcelain", "--untracked-files=all").strip()
        return self.run("revision", "git", "rev-parse", "HEAD").strip()

    def snippet(self, label, document, heading, block=0, **env):
        text = (SOURCE / document).read_text(encoding="utf-8")
        start, = [match for match in re.finditer(r"^#+ " + re.escape(heading) + r"$", text, re.M)]
        section = re.split(r"(?m)^#+ ", text[start.end():], maxsplit=1)[0]
        code = re.findall(r"(?ms)^```sh\n(.*?)^```$", section)[block]
        script = self.evidence / f"{label}.sh"
        script.write_text("set -eu\n" + code, encoding="utf-8")
        return self.run(label, "bash", "-x", script, **env)


def quickstarts(check):
    revision = check.checkout_source((SOLVER,))
    example = Path("examples/runtime/reset_recovery.py")
    # The fresh checkout supplies the committed build inputs. The current
    # documentation example is copied explicitly for precommit admission.
    shutil.copyfile(SOURCE / example, check.checkout / example)
    assert sha(check.checkout / example) == sha(SOURCE / example)
    check.snippet("code-only", GUIDE, "Install the code-only SDK")
    schema = check.checkout / "schemas/engine-config-v1.schema.json"
    dialect = read_json(schema)
    assert dialect["$schema"] == "https://json-schema.org/draft/2020-12/schema"
    assert dialect["$id"] == "urn:engine-sim:config:1" and dialect["properties"]["format_version"]["const"] == 1
    schema_relative = Path("share/engine-sim-runtime/schema/engine-config-v1.schema.json")
    example_relative = Path("share/engine-sim-runtime/example")
    output = check.work / "relocated SDKs Ω"
    output.mkdir()
    rows = []
    for variant in ("static", "shared"):
        bare = check.build / ("sdk-" + variant)
        assert sha(bare / schema_relative) == sha(schema)
        assert not (bare / example_relative).exists() and not (bare / "bin/engine-sim-render").exists()
        check.snippet("example-" + variant, GUIDE, "Opt in to the synthetic example", VARIANT=variant)
        prefix = check.build / ("example-" + variant)
        installed_fixture = prefix / example_relative
        licensed = read_json(check.checkout / "cmake/runtime-example.json")
        expected = {item["path"]: item["sha256"] for item in licensed["files"]}
        assert {file.name: sha(file) for file in installed_fixture.iterdir()} == expected
        assert sha(prefix / schema_relative) == sha(schema)
        relocated = output / ("Runtime " + variant)
        shutil.move(bare, relocated)
        clients = check.snippet("clients-" + variant, GUIDE, "Build the installed clients", VARIANT=variant,
                                SDK_PREFIX=relocated, FIXTURE=installed_fixture)
        assert "88200" in clients and "96000" in clients
        isolated = check.evidence / ("source-hidden-" + variant)
        check.run("isolated-" + variant, sys.executable, "-B", SOURCE / "test/runtime/installed_consumer.py",
                  "--prefix", prefix, "--variant", variant, "--out", isolated)
        assert read_json(isolated / "inside-result.json")["status"] == "pass"
        moved = isolated / "SDK espaço Ω"
        fixture = moved / example_relative
        render_base = check.build / ("render-" + variant)
        check.snippet("render-" + variant, GUIDE, "Render a WAV or load the shared ABI from Python",
                      SDK_PREFIX=moved, FIXTURE=fixture, ES_OUT=render_base)
        pcm = wave(render_base / "offline-run/run.wav", 48000, 96000)
        shutil.copyfile(render_base / "offline-run/run.wav", check.evidence / (variant + "-documented.wav"))
        digest = hashlib.sha256(pcm).hexdigest()
        if variant == "shared":
            cli = check.snippet("ctypes", GUIDE, "Render a WAV or load the shared ABI from Python", block=1,
                                SDK_PREFIX=moved, FIXTURE=fixture)
            assert json_output(cli)["pcm_sha256"] == digest
            reset = check.snippet("reset", GUIDE, "Reproduce reset recovery", SDK_PREFIX=moved, FIXTURE=fixture)
            assert json_output(reset)["failed_resets"] == {"main.json": "CFG_RESOURCE_BYTES", "impulse.wav": "CFG_RESOURCE_BYTES"}
        rows.append({"variant": variant, "pcm_sha256": digest, "schema_sha256": sha(schema),
                     "licensed_fixture": expected, "render_frames": 96000})
    assert rows[0]["pcm_sha256"] == rows[1]["pcm_sha256"]
    return {"revision": revision, "quickstarts": rows, "links": check_links()}


def migration(check, archive_cache):
    revision = check.checkout_source((SOLVER, "piranha"))
    build = check.work / "converter"
    cli = build / "legacy-converter/bin/engine-sim-convert"
    output = check.work / "migration"
    help_text = check.snippet("build-converter", MIGRATION, "Build the offline converter",
                              CONVERTER_OUT=build, ARCHIVE_CACHE=archive_cache)
    assert all(word in help_text for word in ("--selector", "--validate-only", "--batch"))
    check.snippet("convert-and-validate", MIGRATION, "Convert and validate a known engine",
                  CONVERTER=cli, MIGRATION_OUT=output)
    fixture = check.checkout / "examples/runtime/fixture"
    engine = output / "engine.json"
    digest = sha(engine)
    assert digest == "b0d67dae7bab867eed3f79fd9aae581b3eabaac329e0a769c1f82a3408e12bfe"
    for name, status in (("converted", "converted"), ("validated", "validated-only")):
        record = read_json(output / f"{name}.run.json")
        assert record["status"] == status and record["failure"] is None
        assert record["output"]["bytes_sha256"] == digest
    check.snippet("config-json", GUIDE, "Build and load an engine from C++ values")
    check.snippet("programmatic", GUIDE, "Build and load an engine from C++ values", block=1)
    typed = check.build / "model.json"
    assert matching_graph(typed, engine) == 22
    actual = check.build / "migrated.f32"
    loader = check.build / "programmatic-client/engine-sim-file-loader"
    check.run("load-converted", loader, engine, fixture, 44100, "free", actual)
    assert actual.read_bytes() == (check.build / "typed.f32").read_bytes()
    authored = check.build / "authored.f32"
    check.run("load-catalog-fixture", loader, fixture / "main.json", fixture, 44100, "free", authored)
    assert authored.read_bytes() == actual.read_bytes()
    assert actual.stat().st_size == 44100 * 2 * 4
    check.run("converter-recovery", sys.executable, "-B", check.checkout / "test/script/legacy_converter.py",
              cli, fixture, check.work, "cli")
    for item in output.iterdir():
        if item.is_file():
            shutil.copyfile(item, check.evidence / item.name)
    shutil.copyfile(build / "dependency-receipt.json", check.evidence / "dependency-receipt.json")
    shutil.copyfile(typed, check.evidence / "typed-model.json")
    shutil.copyfile(actual, check.evidence / "migrated.f32")
    return {"revision": revision, "canonical_sha256": digest, "typed_graph_nodes": 22,
            "pcm_sha256": sha(actual), "frames": 88200, "links": check_links()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("quickstarts", "migration"))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--deps-root", type=Path, required=True)
    args = parser.parse_args()
    retained = os.environ.get("ENGINE_SIM_GATE_OUTPUT")
    evidence = Path(retained) / ("documentation-" + args.mode) if retained else args.out
    evidence.mkdir(parents=True, exist_ok=not retained)
    if not retained:
        evidence = Path(tempfile.mkdtemp(prefix=args.mode + "-", dir=evidence))
    with tempfile.TemporaryDirectory(prefix=f"engine-sim-doc-{args.mode}-") as directory:
        check = Walkthrough(Path(directory), evidence)
        cache = Path(os.environ.get("ENGINE_SIM_PROFILE_CACHE", str(args.deps_root / "cache")))
        proof = quickstarts(check) if args.mode == "quickstarts" else migration(check, cache)
        save(evidence / "result.json", proof)
        print(json.dumps(proof, indent=2))


if __name__ == "__main__":
    main()
