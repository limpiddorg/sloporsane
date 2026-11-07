"""The fast runner must preserve unittest outcomes, isolation and diagnostics."""

import asyncio
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("tui_test_runner", Path(__file__).parents[1] / "run_tests.py")
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class TestRunnerTest(unittest.TestCase):
    def test_balancing_splits_large_modules_without_losing_or_duplicating_cases(self):
        names = [f"large.Class.test_{index}" for index in range(40)]
        timings = {name: index + 1 for index, name in enumerate(names)}
        groups = runner.batches(names, 4, timings)
        self.assertCountEqual([name for group in groups for name in group], names)
        self.assertTrue(all(groups))
        costs = [sum(timings[name] for name in group) for group in groups]
        self.assertLessEqual(max(costs) - min(costs), max(timings.values()))
        self.assertEqual(runner.batches(names, 1, timings), [names])
        self.assertEqual(runner.batches([], 4, {}), [])

    def test_asyncio_debug_is_explicit_and_each_batch_has_temporary_xdg_storage(self):
        seen = []

        class Probe(unittest.IsolatedAsyncioTestCase):
            async def test_probe(self):
                seen.append((asyncio.get_running_loop().get_debug(), os.environ["XDG_STATE_HOME"]))
                self.addAsyncCleanup(asyncio.sleep, 0)  # Keep unittest's async cleanup lifecycle.

        original = os.environ.get("XDG_STATE_HOME")
        for debug in (False, True):
            with patch.object(unittest.defaultTestLoader, "loadTestsFromNames", return_value=unittest.TestSuite([Probe("test_probe")])):
                result = runner.run_batch(["probe"], debug)
            self.assertTrue(result["successful"], result["output"])
            self.assertEqual(result["timings"][0]["status"], "passed")
        self.assertEqual([debug for debug, _ in seen], [False, True])
        self.assertNotEqual(seen[0][1], seen[1][1])
        self.assertTrue(all(not Path(path).parent.exists() for _, path in seen))
        self.assertEqual(os.environ.get("XDG_STATE_HOME"), original)

    def test_failures_errors_skips_and_subtests_keep_their_exit_status_and_tracebacks(self):
        class Probe(unittest.TestCase):
            def test_failure(self):
                self.fail("failure evidence")

            def test_error(self):
                raise RuntimeError("error evidence")

            def test_skip(self):
                self.skipTest("skip evidence")

            def test_subtest(self):
                with self.subTest(value=1):
                    self.fail("subtest evidence")

            @unittest.expectedFailure
            def test_unexpected_success(self):
                pass

        suite = unittest.defaultTestLoader.loadTestsFromTestCase(Probe)
        with patch.object(unittest.defaultTestLoader, "loadTestsFromNames", return_value=suite):
            result = runner.run_batch(["probe"], False)
        self.assertFalse(result["successful"])
        self.assertEqual(result["tests"], 5)
        self.assertEqual(len(result["failures"]), 2)
        self.assertEqual(len(result["errors"]), 1)
        self.assertEqual(len(result["skipped"]), 1)
        self.assertEqual(len(result["unexpected_successes"]), 1)
        statuses = {item["id"].rsplit(".", 1)[-1]: item["status"] for item in result["timings"]}
        self.assertEqual(statuses, {"test_failure": "failed", "test_error": "error",
                                    "test_skip": "skipped", "test_subtest": "failed",
                                    "test_unexpected_success": "unexpected_success"})
        for message in ("failure evidence", "error evidence", "subtest evidence"):
            self.assertIn(message, result["output"])

    def test_parallel_cli_runs_selected_cases_and_writes_a_report(self):
        with tempfile.TemporaryDirectory(prefix="sos-runner-", dir="/tmp") as directory:
            report = Path(directory) / "report.json"
            process = subprocess.run([sys.executable, str(runner.ROOT / "run_tests.py"), "-j", "2",
                                      "--report", str(report), "test_scan_animation", "test_implementation_diff_order"],
                                     capture_output=True, text=True, timeout=30)
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            data = json.loads(report.read_text())
            self.assertEqual(data["tests"], 11)
            self.assertEqual(len(data["timings"]), 11)
            self.assertEqual(data["jobs"], 2)
            self.assertFalse(data["asyncio_debug"])
            self.assertTrue(data["successful"])
            self.assertTrue(report.with_suffix(".log").is_file())

    def test_unknown_target_is_not_reported_as_an_empty_success(self):
        with self.assertRaises(ValueError):
            runner.discover(["test_does_not_exist"])
