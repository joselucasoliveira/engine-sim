"""Check SDK transfer guards and workflow structure without claiming native CI."""
import argparse
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import sys
import tarfile
import tempfile
import types
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


driver = module("runtime_ci", SOURCE / "tools/runtime_ci.py")
consumer = module("installed_consumer", Path(__file__).with_name("installed_consumer.py"))


def workflow_contract(text):
    starts = list(re.finditer(r"^  ([a-z][a-z_-]*):\s*$", text, re.M))
    jobs = {match.group(1): text[match.end():starts[index + 1].start() if index + 1 < len(starts) else len(text)]
            for index, match in enumerate(starts) if match.group(1) not in ("workflow_dispatch",)}
    if set(jobs) != {"producer", "consumer", "profiles", "diagnostics"}:
        raise ValueError("Required job groups differ")
    for name in ("producer", "consumer", "profiles"):
        oses = re.search(r"^        os: (\[[^\n]+\])", jobs[name], re.M)
        if not oses or set(json.loads(oses.group(1))) != {"ubuntu-24.04", "windows-2022", "macos-15"}:
            raise ValueError("Required native OS matrix differs")
    for name in ("producer", "consumer"):
        variants = re.search(r"^        variant: (\[[^\n]+\])", jobs[name], re.M)
        if not variants or set(json.loads(variants.group(1))) != {"static", "shared"}:
            raise ValueError("Required SDK variant matrix differs")
    for name, profiles in (("producer", {"minimal", "combined"}), ("profiles", {"modules", "converter"})):
        choices = re.search(r"^        profile: (\[[^\n]+\])", jobs[name], re.M)
        if not choices or set(json.loads(choices.group(1))) != profiles:
            raise ValueError("Required acquisition profiles differ")
    for action in re.findall(r"uses: ([^\s]+)", text):
        if not re.fullmatch(r"actions/(checkout|upload-artifact|download-artifact)@[0-9a-f]{40}", action):
            raise ValueError("Unpinned or unexpected action")
    if "contents: read" not in text or "continue-on-error" in text or re.search(r"contents:\s*write", text):
        raise ValueError("Permissions or required failures widened")
    if "actions/checkout@" in jobs["consumer"] or "bootstrap.py" in jobs["consumer"]:
        raise ValueError("Consumer must not check out or bootstrap producer inputs")
    if "submodules: recursive" in text or " compare " not in jobs["consumer"]:
        raise ValueError("Expected selective acquisition and installed SDK comparison")
    if "needs: producer" not in jobs["consumer"] or "always() && !cancelled()" not in text:
        raise ValueError("Consumer failure/independent matrix handling missing")
    for name in jobs:
        if "--hosted" not in jobs[name] or "if: always()" not in jobs[name] or "EXPECTED_REVISION" not in jobs[name]:
            raise ValueError("Native identity or failure artifacts missing")
    if not all("preset: " + preset in jobs["diagnostics"] for preset in ("runtime-asan", "runtime-tsan", "runtime-win-asan")):
        raise ValueError("Separate diagnostics missing")
    return jobs


class TransferTools(unittest.TestCase):
    def setUp(self):
        self.root = WORK / self.id().split(".")[-1]
        self.root.mkdir()

    def test_archive_roundtrip_keeps_bytes_and_inventory(self):
        source = self.root / "source"; source.mkdir()
        (source / "input Ω.bin").write_bytes(bytes(range(256)) * 4097)
        (source / "empty").write_bytes(b"")
        package = self.root / "input.tar"
        driver.archive(source, package)
        destination = self.root / "moved espaço Ω"
        driver.extract(package, destination)
        self.assertEqual(driver.inventory(source), driver.inventory(destination))
        self.assertEqual((source / "input Ω.bin").read_bytes(), (destination / "input Ω.bin").read_bytes())

    def test_unsafe_cross_platform_paths_are_rejected(self):
        for value in ("", ".", "../escape", "/absolute", "C:/absolute", "a\\b", "a/../b", "a/./b", "a//b", "a:stream"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                driver.safe_parts(value)

    def test_symlink_parent_write_is_rejected_before_extracting(self):
        package = self.root / "bad.tar"
        with tarfile.open(package, "w") as stream:
            link = tarfile.TarInfo("link"); link.type = tarfile.SYMTYPE; link.linkname = "inside"; stream.addfile(link)
            item = tarfile.TarInfo("link/child"); item.size = 1; stream.addfile(item, io.BytesIO(b"x"))
        destination = self.root / "output"
        with self.assertRaisesRegex(ValueError, "symlink"):
            driver.extract(package, destination)
        self.assertFalse(destination.exists())

    def test_archive_capacity_is_checked_before_payload_read(self):
        package = self.root / "large.tar"
        item = tarfile.TarInfo("large"); item.size = 1024 * 1024 * 1024 + 1
        package.write_bytes(item.tobuf() + b"\0" * 1024)
        destination = self.root / "output"
        with self.assertRaisesRegex(ValueError, "capacity"):
            driver.extract(package, destination)
        self.assertFalse(destination.exists())

    def test_tampered_transfer_is_rejected_before_build(self):
        payload = self.root / "payload"; payload.mkdir()
        native = {"system": "Linux", "architecture": "x86_64", "compiler": "test identity", "hosted": False}
        files = {name: "0" * 64 for name in ("sdk.tar", "consumers.tar", "runtime_ci.py", "installed_consumer.py")}
        for name in files: (payload / name).write_bytes(b"changed")
        driver.save(payload / "manifest.json", {"version": 1, "revision": "a" * 40, "variant": "static", "dirty": False,
            "native": native, "files": files, "lock_sha256": "b" * 64,
            "dependency_receipt": {"lock_sha256": "b" * 64, "profile": "runtime", "git": {driver.SOLVER: "c" * 40}, "archives": {}}})
        args = types.SimpleNamespace(payload=payload, revision="a" * 40, variant="static", profile="minimal", allow_dirty=False, hosted=False)
        with self.assertRaisesRegex(ValueError, "identity differs"), mock.patch.object(driver, "execute", side_effect=AssertionError("must not build")):
            driver.consume(args, [], native)

    def test_native_tuple_guard_rejects_wrong_arch_and_missing_runner(self):
        # Deliberate guard inputs; these are not native Windows/macOS results.
        with mock.patch.object(driver.platform, "machine", return_value="wrong-architecture"), self.assertRaises(RuntimeError):
            driver.native_tuple(self.root, [])
        with mock.patch.object(driver.platform, "system", return_value="Linux"), mock.patch.object(driver.platform, "machine", return_value="x86_64"), mock.patch.dict(os.environ, {}, clear=True), self.assertRaisesRegex(RuntimeError, "runner identity"):
            driver.native_tuple(self.root, [], hosted=True)

    def test_executable_discovery_handles_both_generators_and_ambiguity(self):
        for system in ("Linux", "Windows"):
            root = self.root / system; root.mkdir()
            filename = "consumer.exe" if system == "Windows" else "consumer"
            with mock.patch.object(consumer.platform, "system", return_value=system):
                single = root / filename; single.write_bytes(b"path-selection fixture")
                self.assertEqual(consumer.executable(root, "consumer"), single)
                single.unlink(); (root / "Release").mkdir()
                multi = root / "Release" / filename; multi.write_bytes(b"path-selection fixture")
                self.assertEqual(consumer.executable(root, "consumer"), multi)
                single.write_bytes(b"ambiguous")
                with self.assertRaises(RuntimeError): consumer.executable(root, "consumer")

    def test_workflow_has_required_isolated_jobs_and_rejects_weakened_controls(self):
        text = (SOURCE / ".github/workflows/runtime-sdk.yml").read_text(encoding="utf-8")
        self.assertEqual(set(workflow_contract(text)), {"producer", "consumer", "profiles", "diagnostics"})
        for changed in (text.replace('variant: ["static", "shared"]', 'variant: ["static"]', 1),
                        text.replace("contents: read", "contents: write"),
                        text.replace("actions/download-artifact@d3f86a106a0bac45b974a628896c90dbdf5c8093", "actions/download-artifact@v4"),
                        text + "\n# continue-on-error: true\n",
                        text.replace('profile: ["minimal", "combined"]', 'profile: ["minimal"]')):
            with self.assertRaises(ValueError): workflow_contract(changed)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not args.out.is_absolute() or args.out.resolve().is_relative_to(SOURCE): parser.error("Use external absolute output")
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="transfer-", dir=args.out) as folder:
        WORK = Path(folder)
        result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(TransferTools))
        driver.save(args.out / "result.json", {"cases": result.testsRun, "failures": len(result.failures), "errors": len(result.errors),
            "skipped": len(result.skipped), "host_python": sys.version, "qualification": "Tool/structure regressions, not hosted native execution"})
        sys.exit(0 if result.wasSuccessful() and result.testsRun == 8 else 1)
