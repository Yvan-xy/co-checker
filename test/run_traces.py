#!/usr/bin/env python3

import argparse
import fnmatch
import subprocess
import sys
from pathlib import Path


class Colors:
    def __init__(self, enabled):
        self.enabled = enabled

    def paint(self, text, code):
        if not self.enabled:
            return text
        return f"\033[{code}m{text}\033[0m"

    def green(self, text):
        return self.paint(text, "32;1")

    def red(self, text):
        return self.paint(text, "31;1")

    def yellow(self, text):
        return self.paint(text, "33;1")

    def cyan(self, text):
        return self.paint(text, "36;1")

    def dim(self, text):
        return self.paint(text, "2")


def parse_args():
    parser = argparse.ArgumentParser(
        allow_abbrev=False,
        description="Run co-checker over selected trace files."
    )
    parser.add_argument(
        "--checker-bin",
        default="build/co-checker",
        help="path to the co-checker executable",
    )
    parser.add_argument(
        "--traces-dir",
        default="traces",
        help="directory containing trace YAML files",
    )
    parser.add_argument(
        "--trace",
        action="append",
        default=[],
        metavar="PATTERN",
        help=(
            "trace selector; may be a file path, basename, or glob pattern. "
            "Can be repeated. Defaults to all *.yaml traces."
        ),
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="list selected traces without running co-checker",
    )
    parser.add_argument(
        "--fail-fast",
        action="store_true",
        help="stop after the first failing co-checker invocation",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="print co-checker stdout/stderr for passing traces too",
    )
    parser.add_argument(
        "--diff",
        nargs=2,
        metavar=("LEFT", "RIGHT"),
        help=(
            "run differential checking with two checker names. "
            "A trace fails if the two checker invocations return different "
            "pass/fail outcomes."
        ),
    )
    parser.add_argument(
        "--no-color",
        action="store_true",
        help="disable colored status output",
    )
    args, co_checker_args = parser.parse_known_args()
    if co_checker_args and co_checker_args[0] == "--":
        co_checker_args = co_checker_args[1:]
    args.co_checker_args = co_checker_args
    return args


def trace_sort_key(path):
    return path.name


def discover_traces(traces_dir):
    return sorted(traces_dir.glob("*.yaml"), key=trace_sort_key)


def matches_selector(trace, selector):
    selector_path = Path(selector)
    if selector_path.exists():
        try:
            return trace.resolve() == selector_path.resolve()
        except OSError:
            return False

    return (
        fnmatch.fnmatch(trace.name, selector)
        or fnmatch.fnmatch(str(trace), selector)
        or trace.name == selector
    )


def select_traces(all_traces, selectors):
    if not selectors:
        return all_traces

    selected = []
    seen = set()
    for selector in selectors:
        matches = [trace for trace in all_traces if matches_selector(trace, selector)]
        if not matches:
            print(f"warning: no traces matched selector `{selector}`", file=sys.stderr)
        for trace in matches:
            resolved = trace.resolve()
            if resolved not in seen:
                selected.append(trace)
                seen.add(resolved)
    return selected


def colored_status(colors, status):
    if status == "PASS":
        return colors.green(status)
    if status == "DIFF":
        return colors.yellow(status)
    if status.startswith("FAIL") or status.startswith("ERROR"):
        return colors.red(status)
    return status


def colored_outcome(colors, outcome):
    if outcome == "pass":
        return colors.green(outcome)
    if outcome == "fail":
        return colors.red(outcome)
    if outcome.startswith("error"):
        return colors.yellow(outcome)
    return outcome


def run_trace(checker_bin, checker_args, trace, verbose, colors):
    cmd = [str(checker_bin), *checker_args, str(trace)]
    completed = subprocess.run(cmd, text=True, capture_output=True)
    status = "PASS" if completed.returncode == 0 else f"FAIL({completed.returncode})"
    print(f"{colored_status(colors, status)} {colors.cyan(str(trace))}")

    if verbose or completed.returncode != 0:
        if completed.stdout:
            print(completed.stdout, end="")
        if completed.stderr:
            print(completed.stderr, end="", file=sys.stderr)

    return completed.returncode


def print_completed_output(completed):
    if completed.stdout:
        print(completed.stdout, end="")
    if completed.stderr:
        print(completed.stderr, end="", file=sys.stderr)


def checker_outcome(returncode):
    if returncode == 0:
        return "pass"
    if returncode == 1:
        return "fail"
    return f"error({returncode})"


def run_checker(checker_bin, checker_name, extra_args, trace):
    cmd = [str(checker_bin), *extra_args, "--checker", checker_name, str(trace)]
    return subprocess.run(cmd, text=True, capture_output=True)


def run_trace_diff(checker_bin, checker_names, extra_args, trace, verbose, colors):
    left_name, right_name = checker_names
    left = run_checker(checker_bin, left_name, extra_args, trace)
    right = run_checker(checker_bin, right_name, extra_args, trace)

    left_outcome = checker_outcome(left.returncode)
    right_outcome = checker_outcome(right.returncode)
    matched = left_outcome == right_outcome
    has_invocation_error = left.returncode not in (0, 1) or right.returncode not in (0, 1)
    status = "PASS" if matched and not has_invocation_error else "DIFF"
    print(
        f"{colored_status(colors, status)} {colors.cyan(str(trace))} "
        f"{left_name}={colored_outcome(colors, left_outcome)} "
        f"{right_name}={colored_outcome(colors, right_outcome)}"
    )

    if verbose or not matched or has_invocation_error:
        print(colors.dim(f"--- {left_name} stdout/stderr ---"))
        print_completed_output(left)
        print(colors.dim(f"--- {right_name} stdout/stderr ---"))
        print_completed_output(right)

    return 0 if matched and not has_invocation_error else 1


def main():
    args = parse_args()
    colors = Colors(enabled=sys.stdout.isatty() and not args.no_color)
    checker_bin = Path(args.checker_bin)
    traces_dir = Path(args.traces_dir)

    if not checker_bin.is_file():
        print(f"co-checker executable not found: {checker_bin}", file=sys.stderr)
        return 2
    if not traces_dir.is_dir():
        print(f"traces directory not found: {traces_dir}", file=sys.stderr)
        return 2

    traces = select_traces(discover_traces(traces_dir), args.trace)
    if not traces:
        print("no traces selected", file=sys.stderr)
        return 2

    if args.list:
        for trace in traces:
            print(trace)
        return 0

    failures = 0
    for trace in traces:
        if args.diff:
            returncode = run_trace_diff(
                checker_bin,
                args.diff,
                args.co_checker_args,
                trace,
                args.verbose,
                colors,
            )
        else:
            returncode = run_trace(
                checker_bin, args.co_checker_args, trace, args.verbose, colors
            )
        if returncode != 0:
            failures += 1
            if args.fail_fast:
                break

    passed = len(traces) - failures
    summary = f"summary: {passed} passed, {failures} failed"
    print(colors.green(summary) if failures == 0 else colors.red(summary))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
