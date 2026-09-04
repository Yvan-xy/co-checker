#!/usr/bin/env python3

import argparse
import csv
import fnmatch
import re
import subprocess
import sys
from pathlib import Path

TRACE_SIZE_RE = re.compile(r"(?P<sessions>\d+)s-(?P<txns>\d+)t-(?P<events>\d+)e")


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
        help="directory containing trace files",
    )
    parser.add_argument(
        "--trace",
        action="append",
        default=[],
        metavar="PATTERN",
        help=(
            "trace selector; may be a file path, basename, or glob pattern. "
            "Can be repeated. Defaults to all top-level traces plus "
            "processed TPC-C traces under the tpcc subdirectory."
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
        "--stat",
        action="store_true",
        help="forward --stat to co-checker and write its numeric statistics to CSV",
    )
    parser.add_argument(
        "--stat-average-only",
        action="store_true",
        help="write only statistic names beginning with average_ to the stat CSV",
    )
    parser.add_argument(
        "--stat-output",
        default="run_traces_stats.csv",
        help="CSV output path used with --stat",
    )
    parser.add_argument(
        "--no-color",
        action="store_true",
        help="disable colored status output",
    )
    args, co_checker_args = parser.parse_known_args()
    if co_checker_args and co_checker_args[0] == "--":
        co_checker_args = co_checker_args[1:]
    if args.stat_average_only and not args.stat:
        parser.error("--stat-average-only requires --stat")
    args.co_checker_args = co_checker_args
    return args


def trace_sort_key(path):
    trace = Path(path)
    match = TRACE_SIZE_RE.search(trace.name)
    if not match:
        return (1, str(trace))

    return (
        0,
        int(match.group("sessions")),
        int(match.group("txns")),
        int(match.group("events")),
        str(trace),
    )


def trace_size_fields(trace):
    match = TRACE_SIZE_RE.search(Path(trace).name)
    if not match:
        return {
            "sessions": "",
            "transactions": "",
            "events": "",
            "average_event_per_txn": "",
        }

    sessions = int(match.group("sessions"))
    txns = int(match.group("txns"))
    events = int(match.group("events"))
    average = events / txns if txns != 0 else 0
    return {
        "sessions": sessions,
        "transactions": txns,
        "events": events,
        "average_event_per_txn": f"{average:.6g}",
    }


def discover_traces(traces_dir):
    traces = list(traces_dir.glob("*.yaml"))
    traces.extend(traces_dir.glob("*.trace"))
    tpcc_dir = traces_dir / "tpcc"
    if tpcc_dir.is_dir():
        traces.extend(tpcc_dir.glob("*.yaml"))
        traces.extend(tpcc_dir.glob("*.trace"))
    return sorted(traces, key=trace_sort_key)


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

    return completed.returncode, completed


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


def checker_elapsed_ms(completed, checker_name):
    pattern = re.compile(rf"^{re.escape(checker_name)}: \w+ \(([^)]+)\)$")
    for line in completed.stdout.splitlines():
        match = pattern.match(line)
        if match:
            return match.group(1)
    return "n/a"


CHECKER_OUTPUT_RE = re.compile(
    r"^(?P<checker>[A-Za-z0-9_.:-]+):\s+"
    r"(?P<outcome>pass|fail|inconclusive|unknown)"
    r"\s+\((?P<elapsed>[0-9]+(?:\.[0-9]+)?)\s*ms\)$"
)
STAT_OUTPUT_RE = re.compile(
    r"^(?P<key>[A-Za-z_][A-Za-z0-9_]*):\s+"
    r"(?P<value>[-+]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)"
    r"(?:[eE][-+]?[0-9]+)?)$"
)


def parse_checker_metrics(completed, expected_checker=None):
    metrics = {}
    pending_stats = {}
    latest_checker = None

    for line in completed.stdout.splitlines():
        checker_match = CHECKER_OUTPUT_RE.match(line)
        if checker_match:
            checker_name = checker_match.group("checker")
            checker_metrics = metrics.setdefault(
                checker_name,
                {"status": "", "elapsed_ms": "", "stats": {}},
            )
            checker_metrics["status"] = checker_match.group("outcome")
            checker_metrics["elapsed_ms"] = checker_match.group("elapsed")
            checker_metrics["stats"].update(pending_stats)
            pending_stats = {}
            latest_checker = checker_name
            continue

        stat_match = STAT_OUTPUT_RE.match(line)
        if stat_match:
            pending_stats[stat_match.group("key")] = stat_match.group("value")

    if pending_stats:
        checker_name = expected_checker or latest_checker
        if checker_name:
            checker_metrics = metrics.setdefault(
                checker_name,
                {"status": "", "elapsed_ms": "", "stats": {}},
            )
            checker_metrics["stats"].update(pending_stats)

    return metrics


def ensure_trace_stat(stat_results, trace_order, trace, trace_status):
    if trace not in stat_results:
        trace_order.append(trace)
        stat_results[trace] = {
            "trace_status": trace_status,
            "checkers": {},
        }
        return
    stat_results[trace]["trace_status"] = trace_status


def record_stat_metrics(
    stat_results,
    trace_order,
    checker_order,
    stat_order,
    trace,
    trace_status,
    metrics,
):
    ensure_trace_stat(stat_results, trace_order, trace, trace_status)
    for checker_name, checker_metrics in metrics.items():
        if checker_name not in checker_order:
            checker_order.append(checker_name)

        stored = stat_results[trace]["checkers"].setdefault(
            checker_name,
            {"status": "", "elapsed_ms": "", "stats": {}},
        )
        if checker_metrics.get("status"):
            stored["status"] = checker_metrics["status"]
        if checker_metrics.get("elapsed_ms"):
            stored["elapsed_ms"] = checker_metrics["elapsed_ms"]
        for stat_name, stat_value in checker_metrics.get("stats", {}).items():
            stat_key = (checker_name, stat_name)
            if stat_key not in stat_order:
                stat_order.append(stat_key)
            stored["stats"][stat_name] = stat_value


def write_stat_csv(
    stat_results,
    trace_order,
    checker_order,
    stat_order,
    path,
    average_only=False,
):
    if average_only:
        stat_order = [
            item for item in stat_order if item[1].startswith("average_")
        ]

    def stat_column(checker_name, stat_name):
        if len(checker_order) == 1:
            return stat_name
        return f"{checker_name}-{stat_name}"

    fieldnames = [
        "trace",
        "sessions",
        "transactions",
        "events",
        "average_event_per_txn",
    ]
    fieldnames.extend(stat_column(checker, stat) for checker, stat in stat_order)

    with Path(path).open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames)
        writer.writeheader()
        for trace in sorted(trace_order, key=trace_sort_key):
            trace_result = stat_results[trace]
            row = {
                "trace": Path(trace).name,
            }
            row.update(trace_size_fields(trace))
            for checker_name, stat_name in stat_order:
                checker_metrics = trace_result["checkers"].get(checker_name, {})
                row[stat_column(checker_name, stat_name)] = checker_metrics.get(
                    "stats", {}
                ).get(stat_name, "")
            writer.writerow(row)


def format_diff_result(colors, checker_name, outcome, elapsed):
    return (
        f"{checker_name}={colored_outcome(colors, outcome)} "
        f"time={elapsed}"
    )


def run_checker(checker_bin, checker_name, extra_args, trace):
    cmd = [str(checker_bin), *extra_args, "--checker", checker_name, str(trace)]
    return subprocess.run(cmd, text=True, capture_output=True)


def run_trace_diff(checker_bin, checker_names, extra_args, trace, verbose, colors):
    left_name, right_name = checker_names
    left = run_checker(checker_bin, left_name, extra_args, trace)
    right = run_checker(checker_bin, right_name, extra_args, trace)

    left_outcome = checker_outcome(left.returncode)
    right_outcome = checker_outcome(right.returncode)
    left_elapsed = checker_elapsed_ms(left, left_name)
    right_elapsed = checker_elapsed_ms(right, right_name)
    matched = left_outcome == right_outcome
    has_invocation_error = left.returncode not in (0, 1) or right.returncode not in (0, 1)
    status = "PASS" if matched and not has_invocation_error else "DIFF"
    print(
        f"{colored_status(colors, status)} {colors.cyan(str(trace))} "
        f"{format_diff_result(colors, left_name, left_outcome, left_elapsed)} "
        f"{format_diff_result(colors, right_name, right_outcome, right_elapsed)}"
    )

    if verbose or not matched or has_invocation_error:
        print(colors.dim(f"--- {left_name} stdout/stderr ---"))
        print_completed_output(left)
        print(colors.dim(f"--- {right_name} stdout/stderr ---"))
        print_completed_output(right)

    returncode = 0 if matched and not has_invocation_error else 1
    return returncode, left, right


def main():
    args = parse_args()
    if args.stat and "--stat" not in args.co_checker_args:
        args.co_checker_args.append("--stat")

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
    stat_results = {}
    stat_trace_order = []
    stat_checker_order = []
    stat_order = []
    for trace in traces:
        if args.diff:
            returncode, left, right = run_trace_diff(
                checker_bin,
                args.diff,
                args.co_checker_args,
                trace,
                args.verbose,
                colors,
            )
            if args.stat:
                trace_status = "PASS" if returncode == 0 else "DIFF"
                for checker_name, completed in zip(args.diff, (left, right)):
                    metrics = parse_checker_metrics(completed, checker_name)
                    if checker_name not in metrics:
                        metrics[checker_name] = {
                            "status": checker_outcome(completed.returncode),
                            "elapsed_ms": "",
                            "stats": {},
                        }
                    record_stat_metrics(
                        stat_results,
                        stat_trace_order,
                        stat_checker_order,
                        stat_order,
                        str(trace),
                        trace_status,
                        metrics,
                    )
        else:
            returncode, completed = run_trace(
                checker_bin, args.co_checker_args, trace, args.verbose, colors
            )
            if args.stat:
                trace_status = (
                    "PASS"
                    if completed.returncode == 0
                    else f"FAIL({completed.returncode})"
                )
                record_stat_metrics(
                    stat_results,
                    stat_trace_order,
                    stat_checker_order,
                    stat_order,
                    str(trace),
                    trace_status,
                    parse_checker_metrics(completed),
                )
        if returncode != 0:
            failures += 1
            if args.fail_fast:
                break

    if args.stat:
        write_stat_csv(
            stat_results,
            stat_trace_order,
            stat_checker_order,
            stat_order,
            args.stat_output,
            args.stat_average_only,
        )
        print(f"stat csv: {args.stat_output}")

    passed = len(traces) - failures
    summary = f"summary: {passed} passed, {failures} failed"
    print(colors.green(summary) if failures == 0 else colors.red(summary))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
