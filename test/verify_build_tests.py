"""Check that CI evidence admission rejects missing, repeated and failed tests."""
import argparse
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("verify_build", SOURCE / "tools/verify_build.py")
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


class Admission(unittest.TestCase):
    def check_xml(self, cases, expected):
        path = WORK / (self.id().split(".")[-1] + ".xml")
        path.write_text("<testsuite>" + cases + "</testsuite>")
        return verify.validate_tests(path, expected)

    def test_accepts_actual_unique_successful_cases(self):
        result = self.check_xml('<testcase name="Known.first"/><testcase name="Known.second"/>', {"Known": 2})
        self.assertEqual(result["cases"], 2)
        self.assertEqual(len(result["xml_sha256"]), 64)

    def test_rejects_empty_green_output(self):
        with self.assertRaises(ValueError):
            self.check_xml("", {"Known": 1})

    def test_rejects_duplicate_names(self):
        with self.assertRaises(ValueError):
            self.check_xml('<testcase name="Known.first"/><testcase name="Known.first"/>', {"Known": 2})

    def test_rejects_skipped_required_case(self):
        with self.assertRaises(ValueError):
            self.check_xml('<testcase name="Known.first"><skipped/></testcase>', {"Known": 1})

    def test_rejects_failed_required_case(self):
        with self.assertRaises(ValueError):
            self.check_xml('<testcase name="Known.first"><failure/></testcase>', {"Known": 1})

    def test_rejects_same_total_from_a_different_suite(self):
        with self.assertRaises(ValueError):
            self.check_xml('<testcase name="Unexpected.first"/>', {"Known": 1})

    def test_desktop_requires_views_without_replacing_legacy_cases(self):
        expected = verify.expected_suites("desktop", False)
        self.assertEqual(expected["HostApplication"], 8)
        self.assertEqual(expected["ViewsApplication"], 4)
        self.assertEqual(expected["RuntimeDesktop"], 5)
        self.assertEqual(expected["DesktopPacing"], 8)
        self.assertEqual(expected["CaptureMechanics"], 1)
        self.assertEqual(expected["DesktopAudio"], 9)
        cases = ''.join(f'<testcase name="{suite}.case{i}"/>'
                        for suite, count in expected.items() for i in range(count))
        self.assertEqual(self.check_xml(cases, expected)["cases"], sum(expected.values()))
        with self.assertRaises(ValueError):
            self.check_xml(cases.replace('<testcase name="ViewsApplication.case0"/>', ''), expected)

    def test_profile_admission_preserves_its_selected_scope(self):
        self.assertNotIn("ViewsApplication", verify.expected_suites("core", False))
        self.assertNotIn("ViewsApplication", verify.expected_suites("desktop-tsan", False))
        self.assertEqual(verify.expected_suites("desktop-tsan", False)["DesktopAudio"], 9)
        self.assertNotIn("DesktopAudio", verify.expected_suites("desktop-windows", True))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    options = parser.parse_args()
    if not options.out.is_absolute() or options.out.resolve().is_relative_to(SOURCE):
        parser.error("out must be absolute and outside the checkout")
    options.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="admission-", dir=options.out) as directory:
        WORK = Path(directory)
        result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Admission))
        (options.out / "result.json").write_text(json.dumps({"cases": result.testsRun, "failures": len(result.failures),
            "errors": len(result.errors), "skipped": len(result.skipped)}, indent=2) + "\n")
        sys.exit(0 if result.wasSuccessful() and result.testsRun == 8 else 1)
