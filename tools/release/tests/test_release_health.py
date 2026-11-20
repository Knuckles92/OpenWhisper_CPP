"""The release health gate over synthetic lifecycle reports: local files only, no network.

from tests/test_release_health.py (OpenWhisper); the fresh-process test of that file is the C++
test ``fresh process workflow exits and preserves user history`` in tests/cli, which runs
fiorivoce-cli --workflow-smoke itself.
"""

import contextlib
import copy
import io
import json
import sys
import unittest

import release_support as rs

import check_release_health as health
from check_release_health import LIMITS, REQUIRED_CHECKS, failures


def _report():
    return dict(schema=1, kind="synthetic_service_lifecycle", passed=True,
                metrics={key: value / 10 for key, value in LIMITS.items()},
                checks=dict.fromkeys(REQUIRED_CHECKS, True))


class TestReleaseHealth(unittest.TestCase):
    def test_missing_or_failed_recovery_proof_cannot_pass_gate(self):
        for name in REQUIRED_CHECKS:
            with self.subTest(name=name):
                report = _report()
                report["checks"].pop(name)
                self.assertTrue(failures(report))
                report["checks"][name] = False
                self.assertTrue(failures(report))

    def test_missing_or_invalid_metrics_fail_closed(self):
        for bad in [None, -1, float("nan"), float("inf"), True, "0.1"]:
            with self.subTest(bad=bad):
                report = _report()
                report["metrics"]["cancel_s"] = bad
                self.assertTrue(failures(report))

    def test_regression_and_absolute_deadline_are_enforced(self):
        baseline = _report()
        self.assertEqual(failures(baseline), [])
        report = _report()
        report["metrics"]["cancel_s"] = 1
        self.assertEqual(failures(report), [])
        self.assertTrue(any("regression" in failure for failure in failures(report, baseline)))
        report["metrics"]["cancel_s"] = 3
        self.assertTrue(any("exceeds" in failure for failure in failures(report)))
        self.assertTrue(failures({}, baseline))

    def test_real_model_report_cannot_pass_synthetic_gate(self):
        report = _report()
        report["kind"] = "real_model"
        self.assertTrue(failures(report))

    def test_noise_floors_keep_tiny_timings_from_failing_the_baseline(self):
        baseline = _report()
        baseline["metrics"]["cancel_s"] = 0.001
        baseline["metrics"]["peak_rss_mb"] = 20
        report = copy.deepcopy(baseline)
        # Ten times slower, but within the 100 ms floor; 49 MB more, within the 50 MB floor.
        report["metrics"]["cancel_s"] = 0.01
        report["metrics"]["peak_rss_mb"] = 69
        self.assertEqual(failures(report, baseline), [])
        report["metrics"]["cancel_s"] = 0.102
        report["metrics"]["peak_rss_mb"] = 71
        self.assertEqual(len(failures(report, baseline)), 2)
        self.assertIn("Baseline is not a valid passing synthetic report",
                      failures(_report(), {"kind": "real_model"}))


class TestCommandLine(rs.TempDirTestCase):
    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                code = health.main([str(arg) for arg in args])
            except SystemExit as exit_:
                code = exit_.code
        return code, err.getvalue()

    def write(self, name, report):
        path = self.tmp / name
        # As fiorivoce-cli saves it: indented, CRLF line ends on Windows.
        path.write_text(json.dumps(report, indent=2), encoding="utf-8")
        return path

    def test_a_passing_report_passes(self):
        self.assertEqual(self.run_main(self.write("smoke.json", _report())), (0, ""))

    def test_a_failing_report_says_why(self):
        report = _report()
        report["metrics"]["restart_s"] = 2.5
        code, err = self.run_main(self.write("smoke.json", report))
        self.assertEqual(code, 1)
        self.assertIn("restart_s: 2.500 exceeds 2", err)

    def test_a_missing_or_unreadable_report_fails_closed(self):
        self.assertEqual(self.run_main(self.tmp / "absent.json")[0], 1)
        broken = self.tmp / "broken.json"
        broken.write_text("{not json", encoding="utf-8")
        self.assertEqual(self.run_main(broken)[0], 1)

    def test_baseline_regression_fails(self):
        baseline = self.write("baseline.json", _report())
        report = _report()
        report["metrics"]["cancel_s"] = 1
        code, err = self.run_main(self.write("smoke.json", report), "--baseline", baseline)
        self.assertEqual(code, 1)
        self.assertIn("regression versus baseline", err)
        self.assertNotEqual(self.run_main(baseline, "--baseline", baseline)[0], 0)

    def test_a_report_or_an_executable_is_required(self):
        self.assertEqual(self.run_main()[0], 2)
        self.assertEqual(self.run_main(self.write("smoke.json", _report()),
                                       "--executable", sys.executable)[0], 2)
        self.assertEqual(self.run_main("--executable", sys.executable, "--timeout", "nan")[0], 2)

    def test_a_failed_smoke_leaves_no_stale_report_that_could_pass(self):
        # A program that does not know --workflow-smoke exits with an error and writes nothing;
        # the passing report left from an earlier run must not be read in its place.
        output = self.write("release-health.json", _report())
        with contextlib.redirect_stderr(io.StringIO()):
            code, err = self.run_main("--executable", sys.executable, "--output", output)
        self.assertEqual(code, 1)
        self.assertIn("Release lifecycle failed or produced no report", err)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
