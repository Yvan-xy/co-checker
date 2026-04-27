#!/usr/bin/env python3

import argparse
import fnmatch
import subprocess
import sys
from pathlib import Path


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
        "--verbose",
        action="store_true",
        help="print co-checker stdout/stderr for passing traces too",
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


def run_trace(checker_bin, checker_args, trace, verbose):
    cmd = [str(checker_bin), *checker_args, str(trace)]
    completed = subprocess.run(cmd, text=True, capture_output=True)
    status = "PASS" if completed.returncode == 0 else f"FAIL({completed.returncode})"
    print(f"{status} {trace}")

    if verbose or completed.returncode != 0:
        if completed.stdout:
            print(completed.stdout, end="")
        if completed.stderr:
            print(completed.stderr, end="", file=sys.stderr)

    return completed.returncode


def main():
    args = parse_args()
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
        returncode = run_trace(checker_bin, args.co_checker_args, trace, args.verbose)
        if returncode != 0:
            failures += 1
            if args.fail_fast:
                break

    print(f"summary: {len(traces) - failures} passed, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
