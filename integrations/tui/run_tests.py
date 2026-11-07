#!/usr/bin/env python3
"""Run the unittest suite in isolated processes, with per-test timing reports."""

import argparse
from concurrent.futures import ProcessPoolExecutor, as_completed
from contextlib import redirect_stderr, redirect_stdout
import io
import json
import multiprocessing
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent
TESTS = ROOT / "tests"
DEFAULT_REPORT = ROOT.parents[1] / "build-ai/test-measurements/tui.json"


def flatten(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from flatten(item)
        else:
            yield item


def discover(targets=(), pattern="test_*.py"):
    # Also used by standard unittest discovery for the fixture imports.
    sys.path.insert(0, str(TESTS))
    loader = unittest.TestLoader()
    if targets:
        names = [name.removesuffix(".py").replace("/", ".") for name in targets]
        suite = loader.loadTestsFromNames(names)
    else:
        suite = loader.discover(str(TESTS), pattern=pattern)
    if loader.errors:
        raise ValueError("\n".join(loader.errors))
    return [test.id() for test in flatten(suite)]


def batches(names, jobs, timings):
    """Balance individual test cases by recorded duration rather than file size."""
    if jobs == 1:
        return [names] if names else []
    groups = [[] for _ in range(min(len(names), jobs * 4))]
    costs = [0.0] * len(groups)
    for name in sorted(names, key=lambda name: -timings.get(name, 1.0)):
        index = min(range(len(groups)), key=costs.__getitem__)
        groups[index].append(name)
        costs[index] += max(timings.get(name, 1.0), 0.001)
    return sorted(groups, key=lambda group: -sum(timings.get(name, 1.0) for name in group))


class TimedResult(unittest.TextTestResult):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.timings = []
        self.outcomes = {}

    def startTest(self, test):
        self.started = time.perf_counter()
        self.cpu_started = time.process_time()
        super().startTest(test)

    def addSuccess(self, test):
        self.outcomes[test.id()] = "passed"
        super().addSuccess(test)

    def addFailure(self, test, err):
        self.outcomes[test.id()] = "failed"
        super().addFailure(test, err)

    def addError(self, test, err):
        self.outcomes[test.id()] = "error"
        super().addError(test, err)

    def addSkip(self, test, reason):
        self.outcomes[test.id()] = "skipped"
        super().addSkip(test, reason)

    def addExpectedFailure(self, test, err):
        self.outcomes[test.id()] = "expected_failure"
        super().addExpectedFailure(test, err)

    def addUnexpectedSuccess(self, test):
        self.outcomes[test.id()] = "unexpected_success"
        super().addUnexpectedSuccess(test)

    def addSubTest(self, test, subtest, err):
        if err is not None:
            self.outcomes[test.id()] = "failed"
        super().addSubTest(test, subtest, err)

    def stopTest(self, test):
        self.timings.append({"id": test.id(), "wall": time.perf_counter() - self.started,
                             "cpu": time.process_time() - self.cpu_started,
                             "status": self.outcomes.get(test.id(), "error")})
        super().stopTest(test)


def run_batch(names, asyncio_debug):
    """Each batch gets its own XDG directories, even when a worker is reused."""
    sys.path.insert(0, str(TESTS))
    setup_runner = unittest.IsolatedAsyncioTestCase._setupAsyncioRunner

    def setup_asyncio(case):
        # unittest forces debug=True; override only that setting, preserving runner cleanup.
        # test_test_runner covers this private hook on Python 3.11+.
        setup_runner(case)
        case._asyncioRunner.get_loop().set_debug(asyncio_debug)

    output = io.StringIO()
    with tempfile.TemporaryDirectory(prefix="sos-tests-", dir="/tmp") as directory:
        environment = {f"XDG_{kind}_HOME": str(Path(directory) / kind.lower())
                       for kind in ("STATE", "CONFIG", "DATA", "CACHE")}
        with patch.dict(os.environ, environment), redirect_stdout(output), redirect_stderr(output):
            with patch.object(unittest.IsolatedAsyncioTestCase, "_setupAsyncioRunner", setup_asyncio):
                suite = unittest.defaultTestLoader.loadTestsFromNames(names)
                result = unittest.TextTestRunner(stream=output, verbosity=2,
                                                resultclass=TimedResult).run(suite)
    return {"tests": result.testsRun, "successful": result.wasSuccessful(),
            "failures": [test.id() for test, _ in result.failures],
            "errors": [test.id() for test, _ in result.errors],
            "skipped": [(test.id(), reason) for test, reason in result.skipped],
            "unexpected_successes": [test.id() for test in result.unexpectedSuccesses],
            "timings": result.timings, "output": output.getvalue()}


def run(groups, jobs, asyncio_debug):
    if jobs == 1:
        for group in groups:
            yield run_batch(group, asyncio_debug)
        return
    # Never fork a parent that has already imported Textual/asyncio in discovery.
    with ProcessPoolExecutor(max_workers=jobs, mp_context=multiprocessing.get_context("spawn")) as pool:
        pending = [pool.submit(run_batch, group, asyncio_debug) for group in groups]
        for future in as_completed(pending):
            yield future.result()


def write_report(path, results, elapsed, jobs, asyncio_debug):
    report = {"elapsed": elapsed, "jobs": jobs, "asyncio_debug": asyncio_debug,
              "tests": sum(result["tests"] for result in results),
              "successful": all(result["successful"] for result in results)}
    for key in ("failures", "errors", "skipped", "unexpected_successes", "timings"):
        report[key] = [entry for result in results for entry in result[key]]
    report["timings"].sort(key=lambda item: -item["wall"])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    path.with_suffix(".log").write_text("\n".join(result["output"] for result in results), encoding="utf-8")
    return report


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("targets", nargs="*", help="unittest names, e.g. test_chat.ChatTest.test_name")
    parser.add_argument("-j", "--jobs", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--asyncio-debug", action="store_true", help="Enable expensive asyncio diagnostics")
    parser.add_argument("--pattern", default="test_*.py", help="Discovery filename glob")
    parser.add_argument("--report", type=Path, default=DEFAULT_REPORT)
    parser.add_argument("--timings-from", type=Path, help="Previous JSON report for better load balancing")
    parser.add_argument("--list", action="store_true", help="List cases without running them")
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    return parser, args


def execute(args, names, started):
    timings = {}
    if args.timings_from:
        previous = json.loads(args.timings_from.read_text(encoding="utf-8"))
        timings = {item["id"]: item["wall"] for item in previous["timings"]}
    groups = batches(names, args.jobs, timings)
    print(f"Running {len(names)} tests with {args.jobs} worker(s); asyncio debug={args.asyncio_debug}", flush=True)
    results = []
    for result in run(groups, args.jobs, args.asyncio_debug):
        results.append(result)
        print(f"{sum(item['tests'] for item in results)}/{len(names)} tests finished", flush=True)
        if not result["successful"]:
            # Keep successful cases in the log, not in a wall of console output.
            details = result["output"]
            separator = details.find("=" * 70)
            print(details[separator:] if separator >= 0 else details, end="", flush=True)
    return write_report(args.report, results, time.perf_counter() - started, args.jobs, args.asyncio_debug)


def summarize(report, path):
    print(f"Ran {report['tests']} tests in {report['elapsed']:.2f}s: "
          f"{len(report['failures'])} failures, {len(report['errors'])} errors, "
          f"{len(report['skipped'])} skipped")
    for item in report["timings"][:10]:
        print(f"  {item['wall']:7.3f}s  {item['id']}")
    print(f"Report: {path}")


def main(argv=None):
    parser, args = parse_args(argv)
    started = time.perf_counter()
    try:
        names = discover(args.targets, args.pattern)
        if not names:
            parser.error("No tests selected")
        if args.list:
            print("\n".join(names))
            return 0
        report = execute(args, names, started)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    summarize(report, args.report)
    return 0 if report["successful"] else 1


if __name__ == "__main__":
    sys.exit(main())
