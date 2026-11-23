"""Exercise the compiled converter from unrelated directories and faulted publications."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


binary, fixture, build = map(Path, sys.argv[1:4])
mode = sys.argv[4]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


with tempfile.TemporaryDirectory(prefix="legacy-cli-", dir=build) as temporary:
    work = Path(temporary)
    source = work / "source"
    shutil.copytree(fixture, source)
    library = source / "library"
    (library / "engine_sim.mr").write_text('public import "runtime_fixture.mr"\n', encoding="utf-8")
    out = work / "output"
    out.mkdir()
    cwd1, cwd2 = work / "cwd1", work / "cwd2"
    cwd1.mkdir()
    cwd2.mkdir()
    entry = source / "main.mr"

    def run(args, expected=0, directory=cwd1, **options):
        argv = [str(binary), *map(str, args)]
        proc = subprocess.run(argv, cwd=directory, capture_output=True, timeout=40, **options)
        if proc.returncode != expected:
            raise RuntimeError(f"{argv!r} returned {proc.returncode}, expected {expected}\n{proc.stderr.decode()}")
        return proc

    def single(target, manifest, entry_file=entry, root=source):
        return ["--entry", entry_file, "--library-root", library, "--asset-root", root,
                "--resource-root", root, "--manifest", manifest,
                *(["--output", target] if target else ["--validate-only"])]

    def verify(manifest, target=None, status="converted"):
        report = json.loads(manifest.read_bytes())
        assert report["record_version"] == 1 and report["status"] == status
        assert report["converter"]["executable_sha256"] == sha(binary)
        assert report["entry"]["sha256"] == sha(Path(report["entry"]["path"]))
        assert report["failure"] is None and report["original"] is None
        assert report["inputs"] and report["resources"]
        for row in [*report["inputs"], *report["resources"]]:
            path = Path(row["canonical_source"])
            assert row["sha256"] == sha(path) and row["length"] == path.stat().st_size
        if target:
            assert report["output"]["path"] == str(target)
            assert report["output"]["bytes_sha256"] == sha(target) == report["output"]["semantic_sha256"]
            assert json.loads(target.read_bytes())["format_version"] == 1
        else:
            assert report["output"]["path"] is None
        return report

    if mode == "cli":
        run(["--help"])
        target, manifest = out / "one.json", out / "one.run.json"
        original = entry.read_bytes()
        run(single(target, manifest))
        first = verify(manifest, target)
        twin, second = out / "two.json", out / "two.run.json"
        run(single(twin, second), directory=cwd2)
        report = verify(second, twin)
        assert twin.read_bytes() == target.read_bytes() and first["inputs"] == report["inputs"]
        assert first["resources"] == report["resources"]

        trace = out / "dry-run.json"
        run(single(None, trace))
        dry = verify(trace, None, "validated-only")
        assert dry["output"]["bytes_sha256"] == sha(target)
        before = target.read_bytes()
        run(single(target, out / "conflicting.json"), 6)
        assert target.read_bytes() == before
        run(single(target, manifest), 6)
        run([*single(target, manifest), "--force"])
        assert target.read_bytes() == before and entry.read_bytes() == original

        # Use named formals through both selector entry points.
        selected = source / "selected.mr"
        text = entry.read_text(encoding="utf-8")
        lines = text.splitlines()
        start = next(i for i, line in enumerate(lines) if line.startswith("import"))
        body = "\n".join(lines[start + 1:]).replace("set_engine(motor)", "")
        body = body.replace('name: "Runtime Single"', "name: title").replace("starter_torque: 40 * u.Nm", "starter_torque: torque")
        selected.write_text(
            lines[start] + "\n\n"
            "public node chosen {\n    input title [string]: \"Default\";\n    input torque [float]: 40.0;\n"
            "    alias output __out: motor;\n" + body + "\n}\n"
            "public node main {\n    input title [string]: \"Default\";\n    input torque [float]: 40.0;\n"
            "    set_engine(chosen(title: title, torque: torque))\n}\n", encoding="utf-8")
        selected_targets = []
        for selector in ("main", "factory:chosen"):
            engine, evidence = out / (selector.split(":")[-1] + ".json"), out / (selector.split(":")[-1] + ".manifest.json")
            run([*single(engine, evidence, selected), "--selector", selector,
                 "--arg", "title=\"Selected legacy motor\"", "--arg", "torque=4.25e1"])
            record = verify(evidence, engine)
            assert record["entry"]["selector"] == selector and record["entry"]["wrapper_sha256"]
            assert record["entry"]["args"] == {"title": "Selected legacy motor", "torque": 42.5}
            result = json.loads(engine.read_bytes())["engine"]
            assert result["name"] == "Selected legacy motor" and result["starter_torque"] == 42.5
            selected_targets.append(engine.read_bytes())
        assert selected_targets[0] == selected_targets[1]
        run([*single(None, out / "unknown.json", selected), "--selector", "factory:unknown"], 2)
        run([*single(None, out / "unknown-arg.json", selected), "--selector", "main", "--arg", "missing=1"], 2)
        run([*single(None, out / "entry-with-arg.json"), "--arg", "torque=1"], 2)
        run([*single(None, out / "too-wide.json", selected), "--selector", "main", "--arg", "torque=92233720368547758080"], 2)
        run([*single(None, out / "bad-arg.json", selected), "--selector", "main", "--arg", 'torque=1,"other":2'], 2)

        imported = library / "objects" / "objects.mr"
        prior = imported.read_bytes()
        run([*single(out / "protected.json", imported), "--force"], 6)
        assert imported.read_bytes() == prior and not (out / "protected.json").exists()
    elif mode == "batch":
        broken = work / "broken"
        shutil.copytree(source, broken)
        (broken / "impulse.wav").write_bytes(b"invalid WAVE fixture payload")
        untouched = out / "untouched.json"
        untouched.write_bytes(b"existing engine bytes\n")
        damaged = out / "late-failure.json"
        run([*single(untouched, damaged, broken / "main.mr", broken), "--force"], 5)
        late = json.loads(damaged.read_bytes())
        assert late["failure"]["code"] == "CFG_RESOURCE_FORMAT" and untouched.read_bytes() == b"existing engine bytes\n"

        def item(key, selected=source, target=None):
            row = {"key": key, "entry": str(selected / "main.mr"), "library_root": str(library),
                   "asset_root": str(selected), "resource_root": str(selected), "selector": "entry", "args": {}}
            row.update({"output": str(target)} if target else {"validate_only": True})
            return row

        def batch(path, items):
            path.write_text(json.dumps({"record_version": 1, "items": items}), encoding="utf-8")

        plan, manifest = out / "plan.json", out / "mixed.json"
        converted = out / "converted.json"
        batch(plan, [item("good", target=converted), item("broken", broken, out / "never.json"), item("dry")])
        run(["--batch", plan, "--manifest", manifest], 8)
        result = json.loads(manifest.read_bytes())
        assert [row["status"] for row in result["items"]] == ["converted", "failed", "validated-only"]
        assert result["exit"] == 8 and result["counts"]["failed"] == 1
        assert not (out / "never.json").exists()
        assert result["items"][0]["output"]["bytes_sha256"] == sha(converted)
        batch(plan, [item("a", target=untouched), item("b", target=untouched)])
        run(["--batch", plan, "--manifest", out / "duplicates.json", "--force"], 2)

        if os.name != "nt":
            import resource

            large = source / "large.mr"
            added = "\n".join(f"port.add_sample({i + 1}.0, 0.001)" for i in range(800))
            large.write_text(entry.read_text(encoding="utf-8").replace(
                "set_engine(motor)", added + "\nset_engine(motor)"), encoding="utf-8")
            limit = 16384
            def small_file_quota():
                resource.setrlimit(resource.RLIMIT_FSIZE, (limit, limit))
            short = out / "short.json"
            run([*single(untouched, short, large), "--force"], 6, preexec_fn=small_file_quota)
            assert untouched.read_bytes() == b"existing engine bytes\n"
            short_result = json.loads(short.read_bytes())
            assert short_result["failure"]["code"] == "CONVERT_SHORT_WRITE"

        # Observe the real first publication and kill the process while it is
        # still journalling the batch. Retry the same invocation without force.
        recovery_plan, recover_manifest = out / "recover-plan.json", out / "recovered.json"
        targets = [out / f"recovery-{i:02}.json" for i in range(24)]
        batch(recovery_plan, [item(f"row-{i:02}", target=path) for i, path in enumerate(targets)])
        args = ["--batch", recovery_plan, "--manifest", recover_manifest]
        child = subprocess.Popen([binary, *map(str, args)], cwd=cwd1, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 35
            while not targets[0].exists() and child.poll() is None and time.monotonic() < deadline:
                time.sleep(0.001)
            assert targets[0].exists() and child.poll() is None and not recover_manifest.exists()
            child.kill()
            child.wait(timeout=5)
            first = targets[0].read_bytes()
            journal = Path(str(recover_manifest) + ".journal")
            assert journal.exists()
            run(args, directory=cwd2)
            assert not journal.exists() and targets[0].read_bytes() == first
            result = json.loads(recover_manifest.read_bytes())
            assert result["counts"]["converted"] == len(targets)
            assert all(row["output"]["bytes_sha256"] == sha(path) for row, path in zip(result["items"], targets))
        finally:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=5)
    else:
        raise ValueError(mode)
