"""Regress installed inventory hashing on the declared Python API baseline."""
import argparse
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("installed_consumer", Path(__file__).with_name("installed_consumer.py"))
consumer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(consumer)


class InventoryHash(unittest.TestCase):
    def test_known_vectors_in_unicode_paths(self):
        for name, data, expected in (
            ("empty", b"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
            ("abc", b"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        ):
            path = WORK / (name + " espaço Ω.bin")
            path.write_bytes(data)
            self.assertEqual(consumer.file_sha256(path), expected)

    def test_large_binary_reads_are_bounded_and_stream_is_closed(self):
        data = bytes(range(256)) * 8192 + b"\0\xffend"
        reads = []

        class Bounded(io.BytesIO):
            def read(self, count=-1):
                self_test.assertGreater(count, 0)
                self_test.assertLessEqual(count, 1024 * 1024)
                reads.append(count)
                return super().read(count)

        self_test = self
        stream = Bounded(data)
        path = types.SimpleNamespace(open=lambda mode: stream if mode == "rb" else None)
        self.assertEqual(consumer.file_sha256(path), hashlib.sha256(data).hexdigest())
        self.assertGreater(len(reads), 2)
        self.assertTrue(stream.closed)

    def test_python_baseline_without_file_digest(self):
        path = WORK / "baseline.bin"
        path.write_bytes(b"abc")
        original = consumer.hashlib
        try:
            # This is an API-surface regression on the current interpreter,
            # not a claim that a separate native Python version executed.
            consumer.hashlib = types.SimpleNamespace(sha256=hashlib.sha256)
            self.assertEqual(consumer.file_sha256(path), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        finally:
            consumer.hashlib = original

    def test_read_failure_propagates_and_closes_stream(self):
        class Broken(io.BytesIO):
            def read(self, count=-1):
                raise OSError("deterministic inventory read failure")

        stream = Broken(b"abc")
        path = types.SimpleNamespace(open=lambda mode: stream)
        with self.assertRaisesRegex(OSError, "inventory read failure"):
            consumer.file_sha256(path)
        self.assertTrue(stream.closed)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not args.out.is_absolute() or args.out.resolve().is_relative_to(SOURCE):
        parser.error("Use an absolute output directory outside the checkout")
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="inventory-", dir=args.out) as folder:
        WORK = Path(folder)
        result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(InventoryHash))
        (WORK / "result.json").write_text(json.dumps({"cases": result.testsRun, "failures": len(result.failures), "errors": len(result.errors),
            "skipped": len(result.skipped), "host_python": sys.version, "baseline_method": "API surface without file_digest"}, indent=2) + "\n", encoding="utf-8")
        # Preserve the small admission summary; disposable input bytes are not
        # evidence of any native SDK execution.
        (args.out / "result.json").write_bytes((WORK / "result.json").read_bytes())
        sys.exit(0 if result.wasSuccessful() and result.testsRun == 4 else 1)
