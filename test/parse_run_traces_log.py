#!/usr/bin/env python3

import argparse
import csv
import re
import sys
from pathlib import Path


ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
TRACE_STATUS_RE = re.compile(
    r"^(?P<status>PASS|DIFF|FAIL\(\d+\)|ERROR[^\s]*)\s+(?P<trace>\S+)(?:\s+(?P<rest>.*))?$"
)
DIFF_RESULT_RE = re.compile(
    r"(?P<checker>[A-Za-z0-9_.:-]+)=(?P<outcome>pass|fail|error\(\d+\))"
    r"(?:\s+time=(?P<elapsed>[0-9]+(?:\.[0-9]+)?|n/a)(?:\s*ms)?)?"
)
CHECKER_OUTPUT_RE = re.compile(
    r"^(?P<checker>[A-Za-z0-9_.:-]+):\s+"
    r"(?P<outcome>pass|fail|inconclusive|unknown)"
    r"(?:\s+\((?P<elapsed>[0-9]+(?:\.[0-9]+)?)\s*ms\))?$"
)
TRACE_SIZE_RE = re.compile(r"(?P<sessions>\d+)s-(?P<txns>\d+)t-(?P<events>\d+)e")


def parse_args():
    parser = argparse.ArgumentParser(
        allow_abbrev=False,
        description="Convert test/run_traces.py output logs to CSV.",
    )
    parser.add_argument(
        "log",
        nargs="?",
        default="-",
        help="run_traces.py log path, or '-' for stdin",
    )
    parser.add_argument(
        "-o",
        "--output",
        default="-",
        help="CSV output path, or '-' for stdout",
    )
    parser.add_argument(
        "--omit-status",
        action="store_true",
        help="omit trace_status and checker status columns from the CSV",
    )
    return parser.parse_args()


def clean_line(line):
    return ANSI_RE.sub("", line.rstrip("\n"))


def parse_elapsed_ms(value):
    if value is None or value == "n/a":
        return ""
    return value


def ensure_trace(results, trace_order, trace, trace_status):
    if trace not in results:
        trace_order.append(trace)
        results[trace] = {
            "trace_status": trace_status,
            "checkers": {},
        }
        return

    if trace_status:
        results[trace]["trace_status"] = trace_status


def record_checker(
    results,
    trace_order,
    checker_order,
    trace,
    trace_status,
    checker,
    outcome,
    elapsed_ms,
):
    ensure_trace(results, trace_order, trace, trace_status)
    if not checker:
        return

    if checker not in checker_order:
        checker_order.append(checker)

    checker_result = results[trace]["checkers"].setdefault(
        checker,
        {
            "status": "",
            "elapsed_ms": "",
        },
    )
    if outcome:
        checker_result["status"] = outcome
    if elapsed_ms:
        checker_result["elapsed_ms"] = elapsed_ms


def parse_trace_status_line(line):
    match = TRACE_STATUS_RE.match(line)
    if not match:
        return None

    rest = match.group("rest") or ""
    diff_results = list(DIFF_RESULT_RE.finditer(rest))
    return {
        "trace_status": match.group("status"),
        "trace": match.group("trace"),
        "diff_results": diff_results,
    }


def parse_log(lines):
    results = {}
    trace_order = []
    checker_order = []
    current_trace = ""
    current_trace_status = ""

    for raw_line in lines:
        line = clean_line(raw_line)
        if not line or line.startswith("summary:"):
            continue

        trace_status = parse_trace_status_line(line)
        if trace_status is not None:
            current_trace = trace_status["trace"]
            current_trace_status = trace_status["trace_status"]
            ensure_trace(
                results,
                trace_order,
                current_trace,
                current_trace_status,
            )

            for result in trace_status["diff_results"]:
                record_checker(
                    results,
                    trace_order,
                    checker_order,
                    current_trace,
                    current_trace_status,
                    result.group("checker"),
                    result.group("outcome"),
                    parse_elapsed_ms(result.group("elapsed")),
                )
            continue

        checker_output = CHECKER_OUTPUT_RE.match(line)
        if checker_output and current_trace:
            record_checker(
                results,
                trace_order,
                checker_order,
                current_trace,
                current_trace_status,
                checker_output.group("checker"),
                checker_output.group("outcome"),
                parse_elapsed_ms(checker_output.group("elapsed")),
            )

    return results, trace_order, checker_order


def read_lines(path):
    if path == "-":
        return sys.stdin
    return Path(path).open("r", encoding="utf-8")


def build_csv_rows(results, trace_order, checker_order):
    rows = []
    for trace in sorted(trace_order, key=trace_sort_key):
        result = results[trace]
        row = {"trace": trace, "trace_status": result["trace_status"]}
        for checker in checker_order:
            checker_result = result["checkers"].get(checker, {})
            row[checker] = checker_result.get("elapsed_ms", "")
        for checker in checker_order:
            checker_result = result["checkers"].get(checker, {})
            row[f"{checker}-status"] = checker_result.get("status", "")
        rows.append(row)
    return rows


def trace_sort_key(trace):
    match = TRACE_SIZE_RE.search(Path(trace).name)
    if not match:
        return (1, trace)

    return (
        0,
        int(match.group("sessions")),
        int(match.group("txns")),
        int(match.group("events")),
        trace,
    )


def write_csv(results, trace_order, checker_order, path, omit_status):
    fieldnames = ["trace", *checker_order]
    if not omit_status:
        fieldnames.append("trace_status")
        fieldnames.extend(f"{checker}-status" for checker in checker_order)
    rows = build_csv_rows(results, trace_order, checker_order)

    if path == "-":
        writer = csv.DictWriter(
            sys.stdout,
            fieldnames=fieldnames,
            extrasaction="ignore",
        )
        writer.writeheader()
        writer.writerows(rows)
        return

    with Path(path).open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=fieldnames,
            extrasaction="ignore",
        )
        writer.writeheader()
        writer.writerows(rows)


def main():
    args = parse_args()
    with read_lines(args.log) as lines:
        results, trace_order, checker_order = parse_log(lines)
    write_csv(results, trace_order, checker_order, args.output, args.omit_status)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
