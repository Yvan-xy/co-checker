#!/usr/bin/env python3
"""Summarize long-running TPC-C result trees without third-party packages.

The runner writes one directory per trial and mode:

    RESULT_ROOT/TRIAL/{baseline,capture-only,order-only,full}

This tool is deliberately tolerant of missing and truncated files so it can be
run while an experiment is in progress.  It writes deterministic JSON and
Markdown: neither output contains an analysis timestamp, and directory walks
and mappings are sorted before serialization.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import re
import statistics
import sys
from typing import Any, Iterable


MODES = ("baseline", "capture-only", "order-only", "full")
CHECKED_MODES = MODES[1:]
INTERVAL_NOTE = (
    "go-tpc interval windows are diagnostics for drift, stalls, and warm-up "
    "effects; windows from one run are not independent experimental replicates "
    "and are not used to compute confidence intervals."
)


def read_text(path: pathlib.Path) -> str | None:
    try:
        return path.read_text(errors="replace")
    except (FileNotFoundError, IsADirectoryError, OSError):
        return None


def read_json(
    path: pathlib.Path, warnings: list[str], label: str
) -> dict[str, Any] | list[Any] | None:
    text = read_text(path)
    if text is None:
        return None
    try:
        return json.loads(text)
    except (json.JSONDecodeError, TypeError) as error:
        warnings.append(f"{label} is not valid JSON: {error}")
        return None


def as_float(value: Any) -> float | None:
    if isinstance(value, bool) or value is None:
        return None
    try:
        return float(str(value).rstrip("%"))
    except (TypeError, ValueError):
        return None


def as_int(value: Any) -> int | None:
    number = as_float(value)
    if number is None or not math.isfinite(number):
        return None
    return int(number)


def round_number(value: float | None, digits: int = 6) -> float | None:
    if value is None or not math.isfinite(value):
        return None
    rounded = round(value, digits)
    return 0.0 if rounded == 0 else rounded


def nearest_rank(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    rank = max(1, math.ceil(fraction * len(ordered)))
    return ordered[rank - 1]


def distribution(values: Iterable[float]) -> dict[str, float | int | None]:
    finite = [value for value in values if math.isfinite(value)]
    if not finite:
        return {
            "count": 0,
            "mean": None,
            "median": None,
            "minimum": None,
            "maximum": None,
            "p05": None,
            "p95": None,
            "population_stddev": None,
            "coefficient_of_variation_percent": None,
        }
    mean = statistics.fmean(finite)
    stddev = statistics.pstdev(finite)
    return {
        "count": len(finite),
        "mean": round_number(mean),
        "median": round_number(statistics.median(finite)),
        "minimum": round_number(min(finite)),
        "maximum": round_number(max(finite)),
        "p05": round_number(nearest_rank(finite, 0.05)),
        "p95": round_number(nearest_rank(finite, 0.95)),
        "population_stddev": round_number(stddev),
        "coefficient_of_variation_percent": round_number(
            stddev * 100.0 / abs(mean) if mean else None
        ),
    }


OPERATION_FIELDS = {
    "Count": ("count", as_int),
    "TPM": ("tpm", as_float),
    "Takes(s)": ("takes_seconds", as_float),
    "Sum(ms)": ("sum_latency_ms", as_float),
    "Avg(ms)": ("average_latency_ms", as_float),
    "Max(ms)": ("maximum_latency_ms", as_float),
    "50th(ms)": ("p50_latency_ms", as_float),
    "90th(ms)": ("p90_latency_ms", as_float),
    "95th(ms)": ("p95_latency_ms", as_float),
    "99th(ms)": ("p99_latency_ms", as_float),
    "99.9th(ms)": ("p99_9_latency_ms", as_float),
}


def normalize_operation(record: dict[str, Any]) -> dict[str, Any]:
    normalized: dict[str, Any] = {}
    known = {"Operation", "Prefix", *OPERATION_FIELDS.keys()}
    for source, (destination, converter) in OPERATION_FIELDS.items():
        if source in record:
            normalized[destination] = converter(record[source])
    extras = {key: record[key] for key in sorted(record) if key not in known}
    if extras:
        normalized["extra_fields"] = extras
    return normalized


def parse_go_tpc(path: pathlib.Path) -> dict[str, Any]:
    text = read_text(path)
    result: dict[str, Any] = {
        "available": text is not None,
        "finished_marker_count": 0,
        "new_order_summary_record_count": 0,
        "final_tpm_c_record_count": 0,
        "json_record_count": 0,
        "ignored_non_json_line_count": 0,
        "malformed_json_candidate_count": 0,
        "intervals": [],
        "summary": {
            "operations": {},
            "tpm_c": None,
            "tpm_total": None,
            "efficiency_percent": None,
        },
        "interval_diagnostics": {
            "note": INTERVAL_NOTE,
            "new_order_tpm": distribution([]),
        },
    }
    if text is None:
        return result

    result["finished_marker_count"] = sum(
        line.strip() == "Finished" for line in text.splitlines()
    )

    intervals: list[dict[str, Any]] = []
    summary_operations: dict[str, Any] = {}
    final: dict[str, Any] = {}
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        # go-tpc emits arrays of JSON objects ("[{") and occasional objects.
        # Timestamped conflict logs also start with "[" and are not JSON
        # candidates; counting them as malformed would reject valid runs.
        if not stripped.startswith(("[{", "{")):
            result["ignored_non_json_line_count"] += 1
            continue
        try:
            decoded = json.loads(stripped)
        except json.JSONDecodeError:
            result["malformed_json_candidate_count"] += 1
            continue
        result["json_record_count"] += 1
        records = decoded if isinstance(decoded, list) else [decoded]
        records = [record for record in records if isinstance(record, dict)]
        current: dict[str, Any] = {}
        for record in records:
            operation = record.get("Operation")
            prefix = str(record.get("Prefix", "")).strip()
            if operation is not None and prefix == "[Current]":
                current[str(operation)] = normalize_operation(record)
            elif operation is not None and prefix == "[Summary]":
                summary_operations[str(operation)] = normalize_operation(record)
                if str(operation) == "NEW_ORDER":
                    result["new_order_summary_record_count"] += 1
            elif "tpmC" in record or "tpmTotal" in record:
                final = record
                if "tpmC" in record:
                    result["final_tpm_c_record_count"] += 1
        if current:
            intervals.append(
                {
                    "index": len(intervals) + 1,
                    "operations": {key: current[key] for key in sorted(current)},
                }
            )

    summary = result["summary"]
    summary["operations"] = {
        key: summary_operations[key] for key in sorted(summary_operations)
    }
    summary["tpm_c"] = as_float(final.get("tpmC"))
    summary["tpm_total"] = as_float(final.get("tpmTotal"))
    summary["efficiency_percent"] = as_float(final.get("efficiency"))
    if summary["tpm_c"] is None:
        summary["tpm_c"] = summary_operations.get("NEW_ORDER", {}).get("tpm")
        summary["tpm_c_source"] = "NEW_ORDER summary TPM fallback"
    else:
        summary["tpm_c_source"] = "go-tpc final tpmC"

    success_count = summary_operations.get("NEW_ORDER", {}).get("count")
    error_count = summary_operations.get("NEW_ORDER_ERR", {}).get("count", 0)
    if success_count is not None and error_count is not None:
        attempted = success_count + error_count
        summary["new_order_success_count"] = success_count
        summary["new_order_error_count"] = error_count
        summary["new_order_error_percent"] = round_number(
            error_count * 100.0 / attempted if attempted else 0.0
        )

    interval_tpm = [
        interval["operations"].get("NEW_ORDER", {}).get("tpm")
        for interval in intervals
    ]
    result["intervals"] = intervals
    result["interval_diagnostics"]["new_order_tpm"] = distribution(
        value for value in interval_tpm if value is not None
    )
    return result


def parse_elapsed(value: str) -> float | None:
    pieces = value.strip().split(":")
    try:
        if len(pieces) == 3:
            return int(pieces[0]) * 3600 + int(pieces[1]) * 60 + float(pieces[2])
        if len(pieces) == 2:
            return int(pieces[0]) * 60 + float(pieces[1])
        if len(pieces) == 1:
            return float(pieces[0])
    except ValueError:
        return None
    return None


DURATION_TOKEN = re.compile(r"([0-9]+(?:\.[0-9]+)?)(ms|h|m|s)")


def parse_duration(value: Any) -> float | None:
    if not isinstance(value, str) or not value:
        return None
    units = {"h": 3600.0, "m": 60.0, "s": 1.0, "ms": 0.001}
    position = 0
    total = 0.0
    for match in DURATION_TOKEN.finditer(value):
        if match.start() != position:
            return None
        total += float(match.group(1)) * units[match.group(2)]
        position = match.end()
    return total if position == len(value) and total > 0 else None


TIME_FIELDS = {
    "User time (seconds)": ("user_seconds", as_float),
    "System time (seconds)": ("system_seconds", as_float),
    "Percent of CPU this job got": ("cpu_percent", as_float),
    "Maximum resident set size (kbytes)": ("maximum_rss_kib", as_int),
    "Major (requiring I/O) page faults": ("major_page_faults", as_int),
    "Minor (reclaiming a frame) page faults": ("minor_page_faults", as_int),
    "Voluntary context switches": ("voluntary_context_switches", as_int),
    "Involuntary context switches": ("involuntary_context_switches", as_int),
    "File system inputs": ("filesystem_inputs", as_int),
    "File system outputs": ("filesystem_outputs", as_int),
    "Exit status": ("exit_status", as_int),
}


def parse_time(path: pathlib.Path) -> dict[str, Any]:
    text = read_text(path)
    result: dict[str, Any] = {"available": text is not None}
    if text is None:
        return result
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("Command being timed:"):
            command = stripped.partition(":")[2].strip()
            result["command"] = command[1:-1] if command.startswith('"') and command.endswith('"') else command
            continue
        elapsed = re.match(
            r"^Elapsed \(wall clock\) time \(h:mm:ss or m:ss\):\s*(\S+)\s*$",
            stripped,
        )
        if elapsed:
            result["elapsed_seconds"] = parse_elapsed(elapsed.group(1))
            continue
        for label, (name, converter) in TIME_FIELDS.items():
            prefix = label + ":"
            if stripped.startswith(prefix):
                result[name] = converter(stripped[len(prefix) :].strip())
                break
    return result


def parse_pidstat(path: pathlib.Path, sample_seconds: float | None) -> dict[str, Any]:
    text = read_text(path)
    result: dict[str, Any] = {
        "available": text is not None,
        "sample_interval_seconds": sample_seconds,
        "samples": [],
    }
    if text is None:
        result["aggregates"] = {}
        return result

    header: list[str] | None = None
    samples: list[dict[str, Any]] = []
    names = {
        "%usr": ("user_cpu_percent", as_float),
        "%system": ("system_cpu_percent", as_float),
        "%wait": ("wait_cpu_percent", as_float),
        "%CPU": ("cpu_percent", as_float),
        "VSZ": ("virtual_size_kib", as_int),
        "RSS": ("rss_kib", as_int),
        "%MEM": ("memory_percent", as_float),
        "kB_rd/s": ("read_kib_per_second", as_float),
        "kB_wr/s": ("write_kib_per_second", as_float),
        "kB_ccwr/s": ("cancelled_write_kib_per_second", as_float),
        "iodelay": ("io_delay", as_int),
        "PID": ("pid", as_int),
    }
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("#"):
            candidate = stripped[1:].split()
            header = candidate if "PID" in candidate and "%CPU" in candidate else header
            continue
        if not header or not stripped or stripped.startswith("Linux"):
            continue
        fields = stripped.split()
        if len(fields) == len(header) + 1 and len(fields) > 1 and fields[1] in {"AM", "PM"}:
            fields[0:2] = [fields[0] + " " + fields[1]]
        if len(fields) > len(header) and header[-1] == "Command":
            fields[len(header) - 1 :] = [" ".join(fields[len(header) - 1 :])]
        if len(fields) != len(header):
            continue
        record = dict(zip(header, fields))
        if record.get("Time") == "Average:" or not record.get("PID", "").isdigit():
            continue
        sample: dict[str, Any] = {"time": record.get("Time")}
        if "Command" in record:
            sample["command"] = record["Command"]
        if "CPU" in record:
            sample["cpu"] = as_int(record["CPU"])
        for source, (destination, converter) in names.items():
            if source in record:
                sample[destination] = converter(record[source])
        samples.append(sample)

    result["samples"] = samples
    aggregates: dict[str, Any] = {}
    for metric in (
        "cpu_percent",
        "user_cpu_percent",
        "system_cpu_percent",
        "wait_cpu_percent",
        "rss_kib",
        "read_kib_per_second",
        "write_kib_per_second",
        "io_delay",
    ):
        values = [as_float(sample.get(metric)) for sample in samples]
        aggregates[metric] = distribution(value for value in values if value is not None)
    if sample_seconds is not None:
        read_rates = [as_float(sample.get("read_kib_per_second")) for sample in samples]
        write_rates = [as_float(sample.get("write_kib_per_second")) for sample in samples]
        aggregates["estimated_read_kib_over_samples"] = round_number(
            sum(value for value in read_rates if value is not None) * sample_seconds
        )
        aggregates["estimated_write_kib_over_samples"] = round_number(
            sum(value for value in write_rates if value is not None) * sample_seconds
        )
        aggregates["io_volume_note"] = (
            "Estimated from sampled pidstat rates and the configured sample interval."
        )
    result["aggregates"] = aggregates
    return result


def parse_key_values(path: pathlib.Path) -> dict[str, Any]:
    text = read_text(path)
    result: dict[str, Any] = {"available": text is not None}
    if text is None:
        return result
    for line in text.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        integer = as_int(value)
        result[key] = integer if integer is not None and str(integer) == value else value
    if isinstance(result.get("shutdown_nanoseconds"), int):
        result["shutdown_seconds"] = round_number(result["shutdown_nanoseconds"] / 1e9)
    return result


def go_tpc_run_evidence(
    raw: dict[str, Any],
    runner_validation: Any,
    requested_duration: Any,
    output_interval: Any,
    timing: dict[str, Any],
    ignore_error_setting: Any,
    silence_errors_setting: Any,
    transaction_weights: Any,
) -> dict[str, Any]:
    expected_seconds = parse_duration(requested_duration)
    interval_seconds = parse_duration(output_interval)
    tolerance = max(2.0, expected_seconds * 0.01) if expected_seconds else None
    new_order = raw["summary"].get("operations", {}).get("NEW_ORDER", {})
    raw_summary_seconds = as_float(new_order.get("takes_seconds"))
    raw_tpm_c = as_float(raw["summary"].get("tpm_c"))
    raw_successes = as_int(raw["summary"].get("new_order_success_count"))
    raw_errors = as_int(raw["summary"].get("new_order_error_count"))
    interval_count = len(raw["intervals"])
    minimum_intervals = (
        max(1, math.floor(expected_seconds / interval_seconds) - 1)
        if expected_seconds and interval_seconds
        else None
    )
    record = runner_validation if isinstance(runner_validation, dict) else {}
    elapsed = as_float(timing.get("elapsed_seconds"))
    command = timing.get("command") if isinstance(timing.get("command"), str) else ""
    expected_weights = (
        ",".join(str(value) for value in transaction_weights)
        if isinstance(transaction_weights, list)
        else None
    )
    checks = {
        "runner_validation_present": isinstance(runner_validation, dict),
        "requested_duration_parsed": expected_seconds is not None,
        "output_interval_parsed": interval_seconds is not None,
        "raw_finished_exactly_once": raw.get("finished_marker_count") == 1,
        "raw_new_order_summary_exactly_once": (
            raw.get("new_order_summary_record_count") == 1
        ),
        "raw_final_tpm_c_exactly_once": raw.get("final_tpm_c_record_count") == 1,
        "raw_json_candidates_well_formed": (
            raw.get("malformed_json_candidate_count") == 0
        ),
        "raw_summary_duration_matches_request": bool(
            expected_seconds is not None
            and raw_summary_seconds is not None
            and tolerance is not None
            and abs(raw_summary_seconds - expected_seconds) <= tolerance
        ),
        "raw_interval_coverage": bool(
            minimum_intervals is not None and interval_count >= minimum_intervals
        ),
        "time_exit_succeeded": timing.get("exit_status") == 0,
        "time_elapsed_matches_request": bool(
            expected_seconds is not None
            and elapsed is not None
            and tolerance is not None
            and abs(elapsed - expected_seconds) <= tolerance
        ),
        "ignore_error_enabled_in_configuration": ignore_error_setting is True,
        "silence_errors_enabled_in_configuration": silence_errors_setting is True,
        "time_command_has_ignore_error": bool(
            re.search(r"(?:^|\s)--ignore-error(?:\s|$)", command)
        ),
        "time_command_has_silence": bool(
            re.search(r"(?:^|\s)--silence(?:\s|$)", command)
        ),
        "time_command_has_requested_duration": bool(
            isinstance(requested_duration, str)
            and re.search(
                rf"(?:^|\s)--time\s+{re.escape(requested_duration)}(?:\s|$)",
                command,
            )
        ),
        "time_command_has_configured_weights": bool(
            expected_weights
            and re.search(
                rf"(?:^|\s)--weight\s+{re.escape(expected_weights)}(?:\s|$)",
                command,
            )
        ),
        "runner_expected_duration_exact": (
            as_float(record.get("expected_seconds")) == expected_seconds
        ),
        "runner_summary_duration_exact": (
            as_float(record.get("summary_seconds")) == raw_summary_seconds
        ),
        "runner_interval_count_exact": (
            as_int(record.get("current_interval_count")) == interval_count
        ),
        "runner_minimum_interval_count_exact": (
            as_int(record.get("minimum_interval_count")) == minimum_intervals
        ),
        "runner_finished_marker_count_exact": (
            as_int(record.get("finished_marker_count"))
            == raw.get("finished_marker_count")
            == 1
        ),
        "runner_malformed_count_exact": (
            as_int(record.get("malformed_json_record_count"))
            == raw.get("malformed_json_candidate_count")
            == 0
        ),
        "runner_tpm_c_exact": (
            as_float(record.get("tpm_c")) == raw_tpm_c and raw_tpm_c is not None
        ),
        "runner_new_order_count_exact": (
            as_int(record.get("new_order_count")) == raw_successes
            and raw_successes is not None
        ),
        "runner_error_count_exact": (
            as_int(record.get("new_order_error_count")) == raw_errors
            and raw_errors is not None
        ),
        "runner_ignore_error_enabled": record.get("go_tpc_ignore_error") is True,
        "runner_silence_errors_enabled": (
            record.get("go_tpc_silence_errors") is True
        ),
    }
    problems = [name for name, passed in checks.items() if passed is not True]
    return {
        "runner_validation": runner_validation,
        "expected_seconds": expected_seconds,
        "output_interval_seconds": interval_seconds,
        "duration_tolerance_seconds": tolerance,
        "raw_summary_seconds": raw_summary_seconds,
        "raw_tpm_c": raw_tpm_c,
        "raw_interval_count": interval_count,
        "minimum_interval_count": minimum_intervals,
        "time_elapsed_seconds": elapsed,
        "expected_transaction_weights": transaction_weights,
        "checks": checks,
        "passed": not problems,
        "problems": problems,
    }


POST_CHECK_PATTERN = re.compile(
    r"^begin to check warehouse (\d+) at condition (3\.3\.2\.(\d+))$"
)
POST_CHECK_FAILURE_MARKERS = ("execute check failed", "panic:", "fatal error")


def post_check_evidence(
    text: str | None, runner_validation: Any, warehouses_value: Any
) -> dict[str, Any]:
    warehouses = as_int(warehouses_value)
    normalized = text or ""
    lower = normalized.lower()
    failure_markers = [
        marker for marker in POST_CHECK_FAILURE_MARKERS if marker in lower
    ]
    finished_count = sum(
        line.strip() == "Finished" for line in normalized.splitlines()
    )
    observed: list[tuple[int, str]] = []
    for line in normalized.splitlines():
        match = POST_CHECK_PATTERN.match(line.strip())
        if match:
            observed.append((int(match.group(1)), match.group(2)))
    expected = (
        {
            (warehouse, f"3.3.2.{condition}")
            for warehouse in range(1, warehouses + 1)
            for condition in range(1, 13)
        }
        if warehouses is not None and warehouses > 0
        else set()
    )
    observed_set = set(observed)
    record = runner_validation if isinstance(runner_validation, dict) else {}
    exact_coverage = bool(
        expected and observed_set == expected and len(observed) == len(expected)
    )
    checks = {
        "runner_validation_present": isinstance(runner_validation, dict),
        "raw_finished_exactly_once": finished_count == 1,
        "raw_has_no_failure_markers": not failure_markers,
        "raw_exact_coverage": exact_coverage,
        "runner_outcome_pass": record.get("outcome") == "pass",
        "runner_warehouses_exact": as_int(record.get("warehouses")) == warehouses,
        "runner_conditions_per_warehouse_exact": (
            as_int(record.get("conditions_per_warehouse")) == 12
        ),
        "runner_check_count_exact": (
            as_int(record.get("checks_completed")) == len(expected) == len(observed)
        ),
        "runner_has_no_failure_markers": record.get("failure_markers") == [],
    }
    problems = [name for name, passed in checks.items() if passed is not True]
    return {
        "runner_validation": runner_validation,
        "warehouses": warehouses,
        "expected_check_count": len(expected),
        "observed_check_count": len(observed),
        "unique_observed_check_count": len(observed_set),
        "finished_marker_count": finished_count,
        "failure_markers": failure_markers,
        "missing_checks": [list(item) for item in sorted(expected - observed_set)],
        "unexpected_checks": [
            list(item) for item in sorted(observed_set - expected)
        ],
        "checks": checks,
        "passed": not problems,
        "problems": problems,
    }


def checker_validation_passed(mode: str, record: Any) -> bool:
    if not isinstance(record, dict) or record.get("mode") != mode:
        return False
    if mode == "baseline":
        return record.get("checker_result") == "disabled"
    expected = {
        "capture-only": "capture-only-complete",
        "order-only": "order-only-complete",
        "full": "pass",
    }[mode]
    if record.get("outcome") != expected or record.get("attempt_conservation") is not True:
        return False
    if record.get("capture_backpressure_wait_count") != 0:
        return False
    if record.get("queue_backpressure_wait_count") != 0:
        return False
    preclose = record.get("native_metrics_before_close")
    if not isinstance(preclose, dict):
        return False
    if as_int(record.get("native_close_nanoseconds")) is None:
        return False
    captured = record.get("captured_transactions")
    if mode in {"order-only", "full"}:
        frontier = record.get("frontier_metrics")
        if not isinstance(frontier, dict):
            return False
        if as_int(frontier.get("updates")) is None or frontier["updates"] <= 0:
            return False
        if frontier.get("errors") != 0:
            return False
        if record.get("ordered_transactions") != captured:
            return False
    if mode == "full":
        return (
            record.get("checked_transactions") == captured
            and record.get("checked_events") == record.get("captured_events")
            and record.get("violating_transactions") == 0
            and as_int(preclose.get("checked_transactions")) is not None
            and preclose["checked_transactions"] > 0
            and as_int(preclose.get("checked_events")) is not None
            and preclose["checked_events"] > 0
        )
    return True


def live_tail_limits(settings: dict[str, Any]) -> dict[str, Any]:
    return {
        "maximum_percent": as_float(settings.get("live_tail_max_percent")),
        "minimum_transactions": as_int(
            settings.get("live_tail_min_transactions")
        ),
        "minimum_events": as_int(settings.get("live_tail_min_events")),
        "maximum_backlog_bytes": as_int(
            settings.get("live_backlog_max_bytes")
        ),
        "maximum_queue_depth": as_int(settings.get("live_queue_max_depth")),
    }


def allowed_tail(total: int | None, minimum: int | None, percent: float | None) -> int | None:
    if total is None or minimum is None or percent is None:
        return None
    if total < 0 or minimum < 0 or not 0 < percent <= 1:
        return None
    return max(minimum, math.ceil(total * percent / 100.0))


def checker_execution_model(mode: str, result: Any) -> str:
    if mode == "baseline":
        return "disabled"
    if mode == "capture-only":
        return "capture-only"
    record = result if isinstance(result, dict) else {}
    preclose = record.get("native_metrics_before_close")
    frontier = record.get("frontier_metrics")
    if (
        isinstance(preclose, dict)
        and isinstance(frontier, dict)
        and (as_int(frontier.get("updates")) or 0) > 0
        and frontier.get("errors") == 0
    ):
        return "live-frontier"
    if isinstance(record.get("native_metrics"), dict):
        return "legacy-deferred" if mode == "full" else "legacy-quiescent-ordering"
    return "unproven"


def checker_completion_tail(result: Any) -> dict[str, Any]:
    record = result if isinstance(result, dict) else {}
    final = record.get("native_metrics")
    before = record.get("native_metrics_before_close")
    final = final if isinstance(final, dict) else {}
    before = before if isinstance(before, dict) else {}

    def difference(total_field: str, completed_field: str) -> int | None:
        total = as_int(before.get(total_field))
        completed = as_int(before.get(completed_field))
        return total - completed if total is not None and completed is not None else None

    captured_transactions = as_int(before.get("captured_transactions"))
    captured_events = as_int(before.get("captured_events"))
    ordered_tail = difference("captured_transactions", "ordered_transactions")
    checked_tail = difference("captured_transactions", "checked_transactions")
    checked_event_tail = difference("captured_events", "checked_events")
    native_close_ns = as_int(record.get("native_close_nanoseconds"))
    return {
        "preclose_metrics_present": bool(before),
        "captured_transactions_before_close": captured_transactions,
        "captured_events_before_close": captured_events,
        "ordered_transactions_before_close": as_int(
            before.get("ordered_transactions")
        ),
        "checked_transactions_before_close": as_int(
            before.get("checked_transactions")
        ),
        "checked_events_before_close": as_int(before.get("checked_events")),
        "ordering_transaction_tail": ordered_tail,
        "checking_transaction_tail": checked_tail,
        "checking_event_tail": checked_event_tail,
        "checking_transaction_tail_percent": round_number(
            checked_tail * 100.0 / captured_transactions
            if checked_tail is not None and captured_transactions
            else None
        ),
        "checking_event_tail_percent": round_number(
            checked_event_tail * 100.0 / captured_events
            if checked_event_tail is not None and captured_events
            else None
        ),
        "queue_depth_before_close": as_int(before.get("queue_depth")),
        "queue_bytes_before_close": as_int(before.get("queue_bytes")),
        "reorder_bytes_before_close": as_int(before.get("reorder_bytes")),
        "capture_active_bytes_before_close": as_int(
            before.get("capture_active_bytes")
        ),
        "final_checked_transactions": as_int(final.get("checked_transactions")),
        "final_checked_events": as_int(final.get("checked_events")),
        "native_close_nanoseconds": native_close_ns,
        "native_close_seconds": round_number(
            native_close_ns / 1e9 if native_close_ns is not None else None
        ),
    }


def raw_checker_evidence(
    mode: str,
    result: Any,
    result_path_exists: bool,
    settings: dict[str, Any],
) -> dict[str, Any]:
    if mode == "baseline":
        checks = {"checker_result_absent": not result_path_exists}
        return {
            "checks": checks,
            "passed": all(checks.values()),
            "problems": [name for name, passed in checks.items() if not passed],
        }

    expected_outcome = {
        "capture-only": "capture-only-complete",
        "order-only": "order-only-complete",
        "full": "pass",
    }[mode]
    expected_native_mode = {"capture-only": 1, "order-only": 2, "full": 3}[mode]
    record = result if isinstance(result, dict) else {}
    adapter = record.get("adapter_metrics")
    native = record.get("native_metrics")
    preclose = record.get("native_metrics_before_close")
    frontier = record.get("frontier_metrics")
    adapter = adapter if isinstance(adapter, dict) else {}
    native = native if isinstance(native, dict) else {}
    preclose = preclose if isinstance(preclose, dict) else {}
    frontier = frontier if isinstance(frontier, dict) else {}
    limits = live_tail_limits(settings)
    registered = as_int(adapter.get("attempts_registered"))
    published = as_int(adapter.get("attempts_published"))
    aborted = as_int(adapter.get("attempts_aborted"))
    captured_transactions = as_int(native.get("captured_transactions"))
    captured_events = as_int(native.get("captured_events"))
    before_captured_transactions = as_int(preclose.get("captured_transactions"))
    before_captured_events = as_int(preclose.get("captured_events"))
    before_ordered_transactions = as_int(preclose.get("ordered_transactions"))
    before_checked_transactions = as_int(preclose.get("checked_transactions"))
    before_checked_events = as_int(preclose.get("checked_events"))
    ordering_tail = (
        before_captured_transactions - before_ordered_transactions
        if before_captured_transactions is not None
        and before_ordered_transactions is not None
        else None
    )
    checking_tail = (
        before_captured_transactions - before_checked_transactions
        if before_captured_transactions is not None
        and before_checked_transactions is not None
        else None
    )
    checking_event_tail = (
        before_captured_events - before_checked_events
        if before_captured_events is not None and before_checked_events is not None
        else None
    )
    transaction_tail_limit = allowed_tail(
        before_captured_transactions,
        limits["minimum_transactions"],
        limits["maximum_percent"],
    )
    event_tail_limit = allowed_tail(
        before_captured_events,
        limits["minimum_events"],
        limits["maximum_percent"],
    )
    checks = {
        "checker_result_is_object": isinstance(result, dict),
        "format_version": record.get("format_version") == 1,
        "mode": record.get("mode") == mode,
        "outcome": record.get("outcome") == expected_outcome,
        "close_status": record.get("close_status") == 0,
        "native_metrics_present": bool(native),
        "adapter_metrics_present": bool(adapter),
        "native_mode": native.get("mode") == expected_native_mode,
        "native_closed": native.get("closed") is True,
        "native_not_fatal": native.get("fatal") is False,
        "no_active_attempts": native.get("active_attempts") == 0,
        "attempt_conservation": bool(
            registered is not None
            and published is not None
            and aborted is not None
            and registered == published + aborted
        ),
        "published_equals_captured_transactions": bool(
            published is not None
            and captured_transactions is not None
            and published == captured_transactions
        ),
        "adapter_events_equal_native_events": bool(
            as_int(adapter.get("captured_events")) is not None
            and as_int(adapter.get("captured_events")) == captured_events
        ),
        "no_adapter_errors": all(
            adapter.get(field) == 0
            for field in ("fatal_calls", "publish_errors", "abort_errors")
        ),
        "no_capture_backpressure": native.get("capture_backpressure_wait_count") == 0,
        "no_queue_backpressure": native.get("queue_backpressure_wait_count") == 0,
        "queue_drained": native.get("queue_depth") == 0,
        "reorder_drained": native.get("reorder_bytes") == 0,
        "native_preclose_metrics_present": bool(preclose),
        "native_close_duration_present": (
            as_int(record.get("native_close_nanoseconds")) is not None
        ),
        "no_capture_after_preclose_snapshot": bool(
            before_captured_transactions is not None
            and before_captured_transactions == captured_transactions
            and before_captured_events is not None
            and before_captured_events == captured_events
        ),
    }
    if mode in {"order-only", "full"}:
        ticks = as_int(frontier.get("ticks"))
        updates = as_int(frontier.get("updates"))
        checks.update(
            {
                "captured_equals_ordered": (
                    captured_transactions is not None
                    and as_int(native.get("ordered_transactions"))
                    == captured_transactions
                ),
                "frontier_metrics_present": bool(frontier),
                "frontier_updates_succeeded": bool(
                    updates is not None
                    and updates > 0
                    and frontier.get("errors") == 0
                    and ticks is not None
                    and ticks >= updates
                    and (as_int(frontier.get("last_timestamp")) or 0) > 0
                ),
                "ordering_ran_before_close": bool(
                    before_ordered_transactions is not None
                    and before_ordered_transactions > 0
                ),
                "ordering_tail_within_limit": bool(
                    ordering_tail is not None
                    and ordering_tail >= 0
                    and transaction_tail_limit is not None
                    and ordering_tail <= transaction_tail_limit
                ),
                "preclose_backlog_within_limits": bool(
                    limits["maximum_queue_depth"] is not None
                    and (as_int(preclose.get("queue_depth")) or 0)
                    <= limits["maximum_queue_depth"]
                    and limits["maximum_backlog_bytes"] is not None
                    and all(
                        value is not None
                        and 0 <= value <= limits["maximum_backlog_bytes"]
                        for value in (
                            as_int(preclose.get("queue_bytes")),
                            as_int(preclose.get("reorder_bytes")),
                            as_int(preclose.get("capture_active_bytes")),
                        )
                    )
                ),
            }
        )
    if mode == "full":
        checks.update(
            {
                "captured_equals_checked": (
                    captured_transactions is not None
                    and as_int(native.get("checked_transactions"))
                    == captured_transactions
                ),
                "captured_events_equal_checked_events": (
                    captured_events is not None
                    and as_int(native.get("checked_events")) == captured_events
                ),
                "zero_violations": native.get("violating_transactions") == 0,
                "verdict_pass": (
                    isinstance(record.get("verdict"), dict)
                    and record["verdict"].get("status") == "pass"
                ),
                "checking_ran_before_close": bool(
                    before_checked_transactions is not None
                    and before_checked_transactions > 0
                    and before_checked_events is not None
                    and before_checked_events > 0
                ),
                "checking_tail_within_limit": bool(
                    checking_tail is not None
                    and checking_tail >= 0
                    and transaction_tail_limit is not None
                    and checking_tail <= transaction_tail_limit
                    and checking_event_tail is not None
                    and checking_event_tail >= 0
                    and event_tail_limit is not None
                    and checking_event_tail <= event_tail_limit
                ),
            }
        )
    problems = [name for name, passed in checks.items() if passed is not True]
    return {
        "execution_model": checker_execution_model(mode, result),
        "live_tail_limits": limits,
        "transaction_tail_limit": transaction_tail_limit,
        "event_tail_limit": event_tail_limit,
        "checks": checks,
        "passed": not problems,
        "problems": problems,
    }


def flattened_checker_matches_raw(mode: str, flattened: Any, raw: Any) -> bool:
    if mode == "baseline":
        return (
            isinstance(flattened, dict)
            and flattened.get("mode") == "baseline"
            and flattened.get("checker_result") == "disabled"
            and raw is None
        )
    if not isinstance(flattened, dict) or not isinstance(raw, dict):
        return False
    adapter = raw.get("adapter_metrics")
    native = raw.get("native_metrics")
    preclose = raw.get("native_metrics_before_close")
    flattened_preclose = flattened.get("native_metrics_before_close")
    if (
        not isinstance(adapter, dict)
        or not isinstance(native, dict)
        or not isinstance(preclose, dict)
        or not isinstance(flattened_preclose, dict)
    ):
        return False
    fields = (
        "captured_transactions",
        "captured_events",
        "ordered_transactions",
        "checked_transactions",
        "checked_events",
        "violating_transactions",
        "capture_backpressure_wait_count",
        "queue_backpressure_wait_count",
    )
    registered = as_int(adapter.get("attempts_registered"))
    published = as_int(adapter.get("attempts_published"))
    aborted = as_int(adapter.get("attempts_aborted"))
    conservation = bool(
        registered is not None
        and published is not None
        and aborted is not None
        and registered == published + aborted
    )
    preclose_fields = (
        "captured_transactions",
        "captured_events",
        "ordered_transactions",
        "checked_transactions",
        "checked_events",
        "queue_depth",
        "queue_bytes",
        "reorder_bytes",
        "capture_active_bytes",
    )
    return bool(
        flattened.get("mode") == mode
        and flattened.get("outcome") == raw.get("outcome")
        and flattened.get("attempt_conservation") == conservation
        and all(flattened.get(field) == native.get(field) for field in fields)
        and flattened.get("native_close_nanoseconds")
        == raw.get("native_close_nanoseconds")
        and all(
            flattened_preclose.get(field) == preclose.get(field)
            for field in preclose_fields
        )
        and (
            mode == "capture-only"
            or flattened.get("frontier_metrics") == raw.get("frontier_metrics")
        )
    )


FINGERPRINT_SETTINGS = (
    "sql_host",
    "database",
    "warehouses",
    "prepare_threads",
    "run_threads",
    "warmup_time",
    "run_time",
    "output_interval",
    "stabilize_seconds",
    "tidb_cpuset",
    "client_cpuset",
    "dsn_params",
    "go_tpc_ignore_error",
    "go_tpc_silence_errors",
    "transaction_weights",
    "queue_capacity_bytes",
    "reorder_capacity_bytes",
    "capture_capacity_bytes",
    "retained_state_capacity_bytes",
    "queue_timeout",
    "capture_timeout",
    "chunk_segment_bytes",
    "max_cached_chunk_segments",
    "monitor_interval_seconds",
    "frontier_interval",
    "live_tail_max_percent",
    "live_tail_min_transactions",
    "live_tail_min_events",
    "live_backlog_max_bytes",
    "live_queue_max_depth",
)


def benchmark_fingerprint(configuration: dict[str, Any] | None) -> dict[str, Any]:
    config = configuration or {}
    settings = config.get("settings") if isinstance(config.get("settings"), dict) else {}
    host = config.get("host") if isinstance(config.get("host"), dict) else {}
    binaries = (
        config.get("binaries") if isinstance(config.get("binaries"), dict) else {}
    )
    sources = config.get("sources") if isinstance(config.get("sources"), dict) else {}

    def binary(name: str) -> dict[str, Any]:
        value = binaries.get(name)
        return value if isinstance(value, dict) else {}

    inputs = {
        "host": {
            key: host.get(key)
            for key in ("node", "platform", "kernel", "cpu_count")
        },
        "sources": {
            key: sources.get(key) for key in ("checker_commit", "tidb_commit")
        },
        "binaries": {
            name: {
                "path": binary(name).get("path"),
                "sha256": binary(name).get("sha256"),
            }
            for name in (
                "tidb_server",
                "go_tpc",
                "native_library",
                "benchmark_runner",
            )
        },
        "settings": {name: settings.get(name) for name in FINGERPRINT_SETTINGS},
        "template": config.get("template"),
    }

    missing: list[str] = []

    def find_missing(value: Any, prefix: str) -> None:
        if isinstance(value, dict):
            for key in sorted(value):
                find_missing(value[key], f"{prefix}.{key}" if prefix else key)
        elif value is None:
            missing.append(prefix)

    find_missing(inputs, "")
    serialized = json.dumps(inputs, sort_keys=True, separators=(",", ":"))
    return {
        "sha256": hashlib.sha256(serialized.encode()).hexdigest(),
        "complete": not missing,
        "missing_fields": missing,
        "inputs": inputs,
    }


def parse_mode(mode_dir: pathlib.Path, root: pathlib.Path, mode: str) -> dict[str, Any]:
    warnings: list[str] = []
    configuration = read_json(mode_dir / "configuration.json", warnings, "configuration.json")
    if not isinstance(configuration, dict):
        configuration = None
    raw_settings = configuration.get("settings") if configuration else None
    settings = raw_settings if isinstance(raw_settings, dict) else {}
    sample_seconds = as_float(settings.get("monitor_interval_seconds"))
    fingerprint = benchmark_fingerprint(configuration)

    workload = parse_go_tpc(mode_dir / "workload.txt")
    warmup = parse_go_tpc(mode_dir / "warmup.txt")
    timings = {
        "prepare": parse_time(mode_dir / "prepare-time.txt"),
        "warmup": parse_time(mode_dir / "warmup-time.txt"),
        "workload": parse_time(mode_dir / "workload-time.txt"),
        "post_check": parse_time(mode_dir / "post-check-time.txt"),
    }
    pidstat = parse_pidstat(mode_dir / "tidb-pidstat.txt", sample_seconds)
    checker_result_path = mode_dir / "checker-result.json"
    checker_result = read_json(checker_result_path, warnings, "checker-result.json")
    checker_runner_validation = read_json(
        mode_dir / "checker-result-validation.json",
        warnings,
        "checker-result-validation.json",
    )
    warmup_runner_validation = read_json(
        mode_dir / "warmup-validation.json", warnings, "warmup-validation.json"
    )
    workload_runner_validation = read_json(
        mode_dir / "workload-validation.json", warnings, "workload-validation.json"
    )
    capability_gate = read_json(
        mode_dir / "capability-gate.json", warnings, "capability-gate.json"
    )
    completed = read_json(mode_dir / "completed.json", warnings, "completed.json")
    checked_shutdown = parse_key_values(mode_dir / "checked-shutdown.txt")
    post_check_shutdown = parse_key_values(mode_dir / "post-check-shutdown.txt")
    post_check_status = read_json(
        mode_dir / "post-check-status.json", warnings, "post-check-status.json"
    )
    post_check_runner_validation = read_json(
        mode_dir / "post-check-validation.json",
        warnings,
        "post-check-validation.json",
    )
    post_check_text = read_text(mode_dir / "post-check.txt")
    workload_status_text = read_text(mode_dir / "workload-exit-status.txt")
    workload_exit_status = as_int(workload_status_text.strip()) if workload_status_text else None

    completion_marker_valid = (
        isinstance(completed, dict)
        and completed.get("schema_version") == 1
        and completed.get("status") == "complete"
    )
    warmup_evidence = go_tpc_run_evidence(
        warmup,
        warmup_runner_validation,
        settings.get("warmup_time"),
        settings.get("output_interval"),
        timings["warmup"],
        settings.get("go_tpc_ignore_error"),
        settings.get("go_tpc_silence_errors"),
        settings.get("transaction_weights"),
    )
    workload_evidence = go_tpc_run_evidence(
        workload,
        workload_runner_validation,
        settings.get("run_time"),
        settings.get("output_interval"),
        timings["workload"],
        settings.get("go_tpc_ignore_error"),
        settings.get("go_tpc_silence_errors"),
        settings.get("transaction_weights"),
    )
    expected_pidstat_samples = (
        max(
            1,
            math.floor(workload_evidence["expected_seconds"] / sample_seconds) - 1,
        )
        if workload_evidence["expected_seconds"] and sample_seconds
        else None
    )
    pidstat_coverage_passed = bool(
        expected_pidstat_samples is not None
        and len(pidstat["samples"]) >= expected_pidstat_samples
    )
    pidstat["coverage"] = {
        "expected_minimum_samples": expected_pidstat_samples,
        "observed_samples": len(pidstat["samples"]),
        "passed": pidstat_coverage_passed,
        "method": (
            "at least floor(requested workload seconds / monitor interval) - 1"
        ),
    }
    execution_model = checker_execution_model(mode, checker_result)
    completion_tail = checker_completion_tail(checker_result)
    completion_tail["server_shutdown_seconds"] = checked_shutdown.get(
        "shutdown_seconds"
    )
    raw_checker = raw_checker_evidence(
        mode, checker_result, checker_result_path.exists(), settings
    )
    runner_validation_passed = checker_validation_passed(
        mode, checker_runner_validation
    )
    checker_validation_matches = flattened_checker_matches_raw(
        mode, checker_runner_validation, checker_result
    )
    post_check = post_check_evidence(
        post_check_text, post_check_runner_validation, settings.get("warehouses")
    )
    post_check_complete = bool(
        post_check["passed"]
        and timings["post_check"].get("exit_status") == 0
        and post_check_shutdown.get("exit_status") == 0
        and isinstance(post_check_status, dict)
    )
    checker_result_present_or_disabled = (
        not checker_result_path.exists()
        if mode == "baseline"
        else isinstance(checker_result, dict)
    )
    checks = {
        "configuration_present": configuration is not None,
        "benchmark_fingerprint_complete": fingerprint["complete"],
        "capability_gate_passed": (
            isinstance(capability_gate, dict) and capability_gate.get("outcome") == "pass"
        ),
        "workload_summary_present": workload["summary"].get("tpm_c") is not None,
        "workload_exit_status": workload_exit_status,
        "workload_succeeded": workload_exit_status == 0,
        "warmup_validation_passed": warmup_evidence["passed"],
        "workload_validation_passed": workload_evidence["passed"],
        "pidstat_coverage_passed": pidstat_coverage_passed,
        "checked_shutdown_succeeded": checked_shutdown.get("exit_status") == 0,
        "checker_result_present_or_disabled": checker_result_present_or_disabled,
        "raw_checker_result_passed": raw_checker["passed"],
        "runner_checker_validation_passed": runner_validation_passed,
        "checker_validation_matches_raw": checker_validation_matches,
        "post_check_raw_and_runner_validation_passed": post_check["passed"],
        "post_check_time_succeeded": timings["post_check"].get("exit_status") == 0,
        "post_check_shutdown_succeeded": post_check_shutdown.get("exit_status") == 0,
        "post_check_status_present": isinstance(post_check_status, dict),
        "post_check_complete": post_check_complete,
        "completion_marker_valid": completion_marker_valid,
    }
    required_boolean_checks = [
        "configuration_present",
        "benchmark_fingerprint_complete",
        "capability_gate_passed",
        "workload_summary_present",
        "workload_succeeded",
        "warmup_validation_passed",
        "workload_validation_passed",
        "pidstat_coverage_passed",
        "checked_shutdown_succeeded",
        "checker_result_present_or_disabled",
        "raw_checker_result_passed",
        "runner_checker_validation_passed",
        "checker_validation_matches_raw",
        "post_check_complete",
        "completion_marker_valid",
    ]
    problems = [name for name in required_boolean_checks if checks[name] is not True]
    checks["all_required_checks_passed"] = not problems
    checks["problems"] = problems
    if problems:
        warnings.append("failed completion checks: " + ", ".join(problems))

    if checks["all_required_checks_passed"]:
        state = "complete"
    elif completion_marker_valid:
        state = "completed-with-validation-errors"
    else:
        state = "partial"
    if not workload["available"]:
        warnings.append("workload.txt is missing")
    if configuration is None:
        warnings.append("configuration.json is missing or unreadable")
    if not completion_marker_valid:
        warnings.append("the mode does not have a valid completed.json marker")
    if execution_model in {"legacy-deferred", "legacy-quiescent-ordering"}:
        warnings.append(
            "legacy result has no live-frontier/pre-close evidence; its measured "
            "throughput is not complete online-checker overhead"
        )

    if mode == "full" and execution_model == "live-frontier":
        throughput_scope = (
            "measured phase includes TiDB execution, capture, packing, queueing, "
            "live commit ordering, and SOWW rule checking"
        )
    elif mode == "full":
        throughput_scope = (
            "legacy measured phase includes capture, packing, queueing, and reorder "
            "retention; live rule checking is unproven and was deferred in the old schema"
        )
    elif mode == "order-only" and execution_model == "live-frontier":
        throughput_scope = (
            "measured phase includes TiDB execution, capture, packing, queueing, "
            "and live commit ordering"
        )
    elif mode == "order-only":
        throughput_scope = (
            "legacy measured phase includes TiDB execution, capture, packing, "
            "queueing, and reorder retention; live ordering is unproven"
        )
    elif mode == "capture-only":
        throughput_scope = "measured phase includes TiDB execution and capture/packing"
    else:
        throughput_scope = "measured phase is TiDB execution with checker disabled"

    return {
        "mode": mode,
        "relative_path": mode_dir.relative_to(root).as_posix(),
        "state": state,
        "configuration": configuration,
        "benchmark_fingerprint": fingerprint,
        "throughput_measurement_scope": throughput_scope,
        "checker_execution_model": execution_model,
        "live_frontier_verified": execution_model == "live-frontier",
        "checking_deferred_to_shutdown": execution_model == "legacy-deferred",
        "completion_tail": completion_tail,
        "workload": workload,
        "warmup": warmup,
        "timings": timings,
        "tidb_pidstat": pidstat,
        "checker_result": checker_result,
        "validation": {
            "checks": checks,
            "capability_gate": capability_gate,
            "warmup": warmup_evidence,
            "workload": workload_evidence,
            "raw_checker_result": raw_checker,
            "checker_result_validation": checker_runner_validation,
            "checker_validation_matches_raw": checker_validation_matches,
            "post_check": post_check,
            "completed": completed,
            "checked_shutdown": checked_shutdown,
            "post_check_shutdown": post_check_shutdown,
            "post_check_status": post_check_status,
        },
        "warnings": sorted(set(warnings)),
    }


def comparison_mismatches(
    baseline: dict[str, Any], checked: dict[str, Any]
) -> list[dict[str, Any]]:
    baseline_inputs = baseline["benchmark_fingerprint"]["inputs"]
    checked_inputs = checked["benchmark_fingerprint"]["inputs"]
    mismatches: list[dict[str, Any]] = []

    def compare(left: Any, right: Any, prefix: str) -> None:
        if isinstance(left, dict) and isinstance(right, dict):
            for key in sorted(set(left) | set(right)):
                compare(
                    left.get(key),
                    right.get(key),
                    f"{prefix}.{key}" if prefix else key,
                )
        elif left != right:
            mismatches.append(
                {"field": prefix, "baseline": left, "checked": right}
            )

    compare(baseline_inputs, checked_inputs, "")
    return mismatches


def compare_to_baseline(
    trial: str, baseline: dict[str, Any], checked: dict[str, Any]
) -> dict[str, Any]:
    baseline_tpm = as_float(baseline["workload"]["summary"].get("tpm_c"))
    checked_tpm = as_float(checked["workload"]["summary"].get("tpm_c"))
    mismatches = comparison_mismatches(baseline, checked)
    measurement_available = (
        baseline_tpm is not None and baseline_tpm > 0 and checked_tpm is not None
    )
    eligible = bool(
        measurement_available
        and baseline["state"] == "complete"
        and checked["state"] == "complete"
        and baseline["benchmark_fingerprint"]["complete"]
        and checked["benchmark_fingerprint"]["complete"]
        and not mismatches
    )
    ineligible_reasons: list[str] = []
    if not measurement_available:
        ineligible_reasons.append("one or both whole-run tpmC measurements are missing")
    if baseline["state"] != "complete":
        ineligible_reasons.append("baseline is not complete")
    if checked["state"] != "complete":
        ineligible_reasons.append("checked mode is not complete")
    if not baseline["benchmark_fingerprint"]["complete"]:
        ineligible_reasons.append("baseline benchmark fingerprint is incomplete")
    if not checked["benchmark_fingerprint"]["complete"]:
        ineligible_reasons.append("checked-mode benchmark fingerprint is incomplete")
    if mismatches:
        ineligible_reasons.append("benchmark fingerprints differ")
    comparison: dict[str, Any] = {
        "trial": trial,
        "mode": checked["mode"],
        "baseline_mode": "baseline",
        "available": eligible,
        "measurement_available": measurement_available,
        "comparable_complete_runs": eligible,
        "ineligible_reasons": ineligible_reasons,
        "configuration_mismatches": mismatches,
        "benchmark_fingerprint_sha256": (
            baseline["benchmark_fingerprint"]["sha256"] if not mismatches else None
        ),
        "baseline_tpm_c": baseline_tpm,
        "checked_tpm_c": checked_tpm,
        "tpm_c_delta": None,
        "throughput_ratio": None,
        "overhead_percent": None,
        "method": (
            "whole-run measured-phase go-tpc tpmC compared with the baseline from "
            "the same trial; this is steady-state throughput overhead. For a "
            "live-frontier full run, rule checking executes concurrently. Native "
            "close and server shutdown are reported separately as end-to-verdict tail."
        ),
        "checked_measurement_scope": checked["throughput_measurement_scope"],
    }
    if eligible:
        comparison["tpm_c_delta"] = round_number(checked_tpm - baseline_tpm)
        comparison["throughput_ratio"] = round_number(checked_tpm / baseline_tpm)
        comparison["overhead_percent"] = round_number(
            (baseline_tpm - checked_tpm) * 100.0 / baseline_tpm
        )
    return comparison


def aggregate_comparisons(comparisons: list[dict[str, Any]]) -> dict[str, Any]:
    aggregate: dict[str, Any] = {}
    for mode in CHECKED_MODES:
        matching = [
            item
            for item in comparisons
            if item["mode"] == mode and item["comparable_complete_runs"]
        ]
        by_fingerprint: dict[str, list[dict[str, Any]]] = {}
        for item in matching:
            by_fingerprint.setdefault(
                item["benchmark_fingerprint_sha256"], []
            ).append(item)
        compatible_groups: list[dict[str, Any]] = []
        for fingerprint in sorted(by_fingerprint):
            group = by_fingerprint[fingerprint]
            overheads = [item["overhead_percent"] for item in group]
            ratios = [item["throughput_ratio"] for item in group]
            compatible_groups.append(
                {
                    "benchmark_fingerprint_sha256": fingerprint,
                    "matched_trial_count": len(group),
                    "trials": [item["trial"] for item in group],
                    "overhead_percent": distribution(
                        value for value in overheads if value is not None
                    ),
                    "throughput_ratio": distribution(
                        value for value in ratios if value is not None
                    ),
                }
            )
        aggregate[mode] = {
            "matched_trial_count": len(matching),
            "compatible_fingerprint_group_count": len(compatible_groups),
            "compatible_fingerprint_groups": compatible_groups,
            "groups_are_not_combined": True,
            "replicate_unit": "one complete matched trial, not an interval window",
        }
    return aggregate


def discover(root: pathlib.Path) -> list[tuple[str, pathlib.Path, str]]:
    discovered: list[tuple[str, pathlib.Path, str]] = []
    if root.name in MODES and root.is_dir():
        discovered.append((root.parent.name, root, root.name))
        return discovered
    for trial_dir in sorted(root.iterdir() if root.is_dir() else [], key=lambda path: path.name):
        if not trial_dir.is_dir():
            continue
        for mode in MODES:
            mode_dir = trial_dir / mode
            if mode_dir.is_dir():
                discovered.append((trial_dir.name, mode_dir, mode))
    return discovered


def analyze(root: pathlib.Path) -> dict[str, Any]:
    root = root.resolve()
    grouped: dict[str, list[dict[str, Any]]] = {}
    for trial, mode_dir, mode in discover(root):
        grouped.setdefault(trial, []).append(parse_mode(mode_dir, root, mode))

    trials: list[dict[str, Any]] = []
    comparisons: list[dict[str, Any]] = []
    root_warnings: list[str] = []
    if not grouped:
        root_warnings.append("no trial/mode directories were discovered")
    for trial in sorted(grouped):
        modes = sorted(grouped[trial], key=lambda item: MODES.index(item["mode"]))
        by_mode = {item["mode"]: item for item in modes}
        baseline = by_mode.get("baseline")
        trial_warnings: list[str] = []
        if baseline is None:
            trial_warnings.append("same-trial baseline is missing; overhead cannot be calculated")
        for mode in CHECKED_MODES:
            checked = by_mode.get(mode)
            if checked is not None and baseline is not None:
                item = compare_to_baseline(trial, baseline, checked)
                checked["comparison_to_same_trial_baseline"] = item
                comparisons.append(item)
            elif checked is not None:
                checked["comparison_to_same_trial_baseline"] = {
                    "trial": trial,
                    "mode": mode,
                    "available": False,
                    "reason": "same-trial baseline is missing",
                }
        trials.append(
            {
                "trial": trial,
                "modes": modes,
                "missing_modes": [mode for mode in MODES if mode not in by_mode],
                "warnings": trial_warnings,
            }
        )

    return {
        "schema_version": 1,
        "result_root": root.as_posix(),
        "methodology": {
            "comparison": (
                "Each checked mode is compared only with a complete, fingerprint-"
                "compatible whole-run baseline from the same trial. Positive "
                "overhead means lower measured-phase throughput. A headline-eligible "
                "full run must prove that the live frontier advanced, rule checking "
                "ran before close, and only a small bounded tail remained."
            ),
            "interval_windows": INTERVAL_NOTE,
            "partial_results": (
                "A partial mode is reported rather than discarded; it is excluded "
                "from overhead headlines and aggregate matched-trial statistics."
            ),
            "aggregate_grouping": (
                "Matched trials are aggregated only within an identical benchmark "
                "fingerprint; different hosts, binaries, runners, capacities, or "
                "workload settings are never pooled."
            ),
            "resource_scope": (
                "pidstat CPU, RSS, and I/O are sampled only during the measured "
                "workload, so concurrent ordering/checking is included. Native-close "
                "and whole-server shutdown durations are separate end-to-verdict "
                "measurements and are not folded into tpmC."
            ),
            "checker_counter_scope": (
                "Checker counters cover the TiDB server lifetime, including prepare, "
                "warm-up, and measured phases. Without phase-boundary counters they "
                "must not be attributed solely to measured NewOrder transactions."
            ),
            "legacy_results": (
                "Results without native_metrics_before_close and successful "
                "frontier_metrics are labeled legacy deferred/quiescent runs and "
                "are excluded from complete online-order/full overhead headlines."
            ),
        },
        "trials": trials,
        "comparisons": comparisons,
        "aggregate_comparisons": aggregate_comparisons(comparisons),
        "warnings": root_warnings,
    }


def format_number(value: Any, digits: int = 2, suffix: str = "") -> str:
    number = as_float(value)
    return "—" if number is None else f"{number:.{digits}f}{suffix}"


def markdown(summary: dict[str, Any]) -> str:
    lines = [
        "# Long-running TPC-C benchmark summary",
        "",
        f"Result root: `{summary['result_root']}`",
        "",
        "## Methodology",
        "",
        summary["methodology"]["comparison"],
        "",
        f"**Important:** {summary['methodology']['interval_windows']}",
        "",
        summary["methodology"]["aggregate_grouping"],
        "",
        summary["methodology"]["resource_scope"],
        "",
        summary["methodology"]["checker_counter_scope"],
        "",
        summary["methodology"]["legacy_results"],
        "",
    ]
    if summary["warnings"]:
        lines.extend(["## Root warnings", ""])
        lines.extend(f"- {warning}" for warning in summary["warnings"])
        lines.append("")

    for trial in summary["trials"]:
        lines.extend([f"## Trial `{trial['trial']}`", ""])
        reference = next(
            (mode for mode in trial["modes"] if mode["mode"] == "baseline"),
            trial["modes"][0],
        )
        reference_settings = (reference.get("configuration") or {}).get("settings")
        settings = reference_settings if isinstance(reference_settings, dict) else {}
        lines.extend(
            [
                "Configuration: {warehouses} warehouse(s), {threads} client thread(s), "
                "{warmup} warm-up, {runtime} measured run, {interval} output interval; "
                "TiDB CPUs `{tidb_cpus}`, client CPUs `{client_cpus}`.".format(
                    warehouses=settings.get("warehouses", "unknown"),
                    threads=settings.get("run_threads", "unknown"),
                    warmup=settings.get("warmup_time", "unknown"),
                    runtime=settings.get("run_time", "unknown"),
                    interval=settings.get("output_interval", "unknown"),
                    tidb_cpus=settings.get("tidb_cpuset", "unknown"),
                    client_cpus=settings.get("client_cpuset", "unknown"),
                ),
                "",
                "| Mode | State | Checker execution | Measured-phase tpmC | Steady-state overhead vs baseline | Errors | Error rate | NEW_ORDER p99 | TiDB CPU mean | Sampled measured-phase TiDB max RSS | Server shutdown/end-to-result | Native close tail | Post-check |",
                "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
            ]
        )
        for mode in trial["modes"]:
            workload = mode["workload"]["summary"]
            comparison = mode.get("comparison_to_same_trial_baseline", {})
            overhead = (
                "baseline"
                if mode["mode"] == "baseline"
                else format_number(comparison.get("overhead_percent"), 2, "%")
            )
            pidstat = mode["tidb_pidstat"].get("aggregates", {})
            cpu = pidstat.get("cpu_percent", {}).get("mean")
            rss = pidstat.get("rss_kib", {}).get("maximum")
            rss_mib = rss / 1024.0 if rss is not None else None
            shutdown = mode["validation"]["checked_shutdown"].get("shutdown_seconds")
            native_close = mode["completion_tail"].get("native_close_seconds")
            p99 = workload.get("operations", {}).get("NEW_ORDER", {}).get(
                "p99_latency_ms"
            )
            errors = workload.get("new_order_error_count")
            error_rate = workload.get("new_order_error_percent")
            post = "pass" if mode["validation"]["checks"]["post_check_complete"] else "incomplete"
            lines.append(
                "| {mode} | {state} | {execution} | {tpm} | {overhead} | {errors} | {error_rate} | {p99} | {cpu} | {rss} | {shutdown} | {native_close} | {post} |".format(
                    mode=mode["mode"],
                    state=mode["state"],
                    execution=mode["checker_execution_model"],
                    tpm=format_number(workload.get("tpm_c")),
                    overhead=overhead,
                    errors=errors if errors is not None else "—",
                    error_rate=format_number(error_rate, 3, "%"),
                    p99=format_number(p99, 2, " ms"),
                    cpu=format_number(cpu, 2, "%"),
                    rss=format_number(rss_mib, 1, " MiB"),
                    shutdown=format_number(shutdown, 3, " s"),
                    native_close=format_number(native_close, 3, " s"),
                    post=post,
                )
            )
        lines.extend(
            [
                "",
                "For `live-frontier` full mode, measured tpmC includes concurrent "
                "capture, ordering, and rule checking. Native close and whole-server "
                "shutdown are end-to-verdict tail latencies, shown separately instead "
                "of being silently folded into steady-state tpmC.",
                "",
            ]
        )

        lines.extend(
            [
                "### Interval diagnostics",
                "",
                "These describe within-run behavior only; the rows are not replicate experiments.",
                "",
                "| Mode | Windows | Mean tpm | Min tpm | P95 tpm | Max tpm | CV |",
                "|---|---:|---:|---:|---:|---:|---:|",
            ]
        )
        for mode in trial["modes"]:
            stats = mode["workload"]["interval_diagnostics"]["new_order_tpm"]
            lines.append(
                "| {mode} | {count} | {mean} | {minimum} | {p95} | {maximum} | {cv} |".format(
                    mode=mode["mode"],
                    count=stats["count"],
                    mean=format_number(stats["mean"]),
                    minimum=format_number(stats["minimum"]),
                    p95=format_number(stats["p95"]),
                    maximum=format_number(stats["maximum"]),
                    cv=format_number(stats["coefficient_of_variation_percent"], 2, "%"),
                )
            )
        lines.append("")

        checked_with_results = [
            mode for mode in trial["modes"] if isinstance(mode.get("checker_result"), dict)
        ]
        if checked_with_results:
            lines.extend(
                [
                    "### Checker results",
                    "",
                    "| Mode | Outcome | Captured txns | Events | Ordered txns | Checked txns | Violations | Checking | Peak capture | Peak reorder |",
                    "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
                ]
            )
            for mode in checked_with_results:
                checker = mode["checker_result"]
                native = checker.get("native_metrics", {})
                checking_seconds = (
                    as_float(native.get("checking_ns")) / 1e9
                    if as_float(native.get("checking_ns")) is not None
                    else None
                )
                capture_mib = (
                    as_float(native.get("capture_peak_bytes")) / 1048576
                    if as_float(native.get("capture_peak_bytes")) is not None
                    else None
                )
                reorder_mib = (
                    as_float(native.get("reorder_peak_bytes")) / 1048576
                    if as_float(native.get("reorder_peak_bytes")) is not None
                    else None
                )
                lines.append(
                    "| {mode} | {outcome} | {captured} | {events} | {ordered} | {checked} | {violations} | {checking} | {capture} | {reorder} |".format(
                        mode=mode["mode"],
                        outcome=checker.get("outcome", "—"),
                        captured=native.get("captured_transactions", "—"),
                        events=native.get("captured_events", "—"),
                        ordered=native.get("ordered_transactions", "—"),
                        checked=native.get("checked_transactions", "—"),
                        violations=native.get("violating_transactions", "—"),
                        checking=format_number(checking_seconds, 3, " s"),
                        capture=format_number(capture_mib, 1, " MiB"),
                        reorder=format_number(reorder_mib, 1, " MiB"),
                    )
                )
            lines.append("")

        warnings: list[str] = list(trial["warnings"])
        if trial["missing_modes"]:
            warnings.append("modes not present: " + ", ".join(trial["missing_modes"]))
        for mode in trial["modes"]:
            warnings.extend(f"{mode['mode']}: {warning}" for warning in mode["warnings"])
            comparison = mode.get("comparison_to_same_trial_baseline")
            if comparison and comparison.get("configuration_mismatches"):
                warnings.append(
                    f"{mode['mode']}: comparison has configuration mismatches; see summary.json"
                )
            if comparison and not comparison.get("available", False):
                reasons = comparison.get("ineligible_reasons")
                if reasons:
                    warnings.append(
                        f"{mode['mode']}: overhead is not headline-eligible: "
                        + "; ".join(reasons)
                    )
        if warnings:
            lines.extend(["### Warnings", ""])
            lines.extend(f"- {warning}" for warning in warnings)
            lines.append("")

    lines.extend(
        [
            "## Matched-trial aggregate",
            "",
            "Only complete, identical-fingerprint whole runs contribute. Fingerprint groups are not pooled.",
            "",
            "| Mode | Fingerprint | Matched trials | Median overhead | Min overhead | Max overhead |",
            "|---|---:|---:|---:|---:|---:|",
        ]
    )
    for mode in CHECKED_MODES:
        aggregate = summary["aggregate_comparisons"][mode]
        groups = aggregate["compatible_fingerprint_groups"]
        if not groups:
            lines.append(f"| {mode} | — | 0 | — | — | — |")
        for group in groups:
            stats = group["overhead_percent"]
            lines.append(
                "| {mode} | `{fingerprint}` | {count} | {median} | {minimum} | {maximum} |".format(
                    mode=mode,
                    fingerprint=group["benchmark_fingerprint_sha256"][:12],
                    count=group["matched_trial_count"],
                    median=format_number(stats["median"], 2, "%"),
                    minimum=format_number(stats["minimum"], 2, "%"),
                    maximum=format_number(stats["maximum"], 2, "%"),
                )
            )
    lines.append("")
    return "\n".join(lines)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze result trees written by run_tpcc_long_mode.sh."
    )
    parser.add_argument("result_root", type=pathlib.Path)
    parser.add_argument(
        "--json-output",
        type=pathlib.Path,
        help="JSON destination (default: RESULT_ROOT/summary.json)",
    )
    parser.add_argument(
        "--markdown-output",
        type=pathlib.Path,
        help="Markdown destination (default: RESULT_ROOT/summary.md)",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    root = args.result_root.resolve()
    if not root.is_dir():
        print(f"result root is not a directory: {root}", file=sys.stderr)
        return 2
    json_output = (args.json_output or root / "summary.json").resolve()
    markdown_output = (args.markdown_output or root / "summary.md").resolve()
    summary = analyze(root)
    json_output.parent.mkdir(parents=True, exist_ok=True)
    markdown_output.parent.mkdir(parents=True, exist_ok=True)
    json_output.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    markdown_output.write_text(markdown(summary))
    print(f"wrote {json_output}")
    print(f"wrote {markdown_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
