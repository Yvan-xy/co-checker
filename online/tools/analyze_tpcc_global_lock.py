#!/usr/bin/env python3
"""Analyze paired pristine/serialized-global-lock TPC-C experiments.

The runner records an append-only attempt tree:

    RESULT_ROOT/trials/trial-NNN/MODE/attempt-NNN/

This analyzer is intentionally dependency-free and fail-closed.  The fixed
trial/mode matrix comes from ``experiment.json`` rather than directory
discovery, so an attempt that never started still produces an invalid CSV row.
For a mode with retries, the newest valid completed attempt wins; when none is
valid, the newest attempt is retained for diagnostics.
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import math
import os
import pathlib
import re
import statistics
import sys
import tempfile
from typing import Any, Iterable


MODES = (
    "baseline-original",
    "serialized-capture-only",
    "serialized-order-only",
    "serialized-full",
)
CHECKED_MODES = MODES[1:]
PIPELINE_MODE = {
    "serialized-capture-only": "capture-only",
    "serialized-order-only": "order-only",
    "serialized-full": "full",
}
NATIVE_MODE = {
    "serialized-capture-only": 1,
    "serialized-order-only": 2,
    "serialized-full": 3,
}
EXPECTED_OUTCOME = {
    "serialized-capture-only": "capture-only-complete",
    "serialized-order-only": "order-only-complete",
    "serialized-full": "pass",
}
ATTEMPT_PATTERN = re.compile(r"^attempt-(\d+)$")
CHECKER_METRIC_SCOPE = "epoch-wide: prepare + warm-up + measured workload"
ATTEMPT_ARTIFACTS = {
    "configuration": "configuration.json",
    "runner_status": "runner-status.json",
    "completed": "completed.json",
    "prepare_validation": "prepare-validation.json",
    "prepare_status": "prepare-status.json",
    "warmup_validation": "warmup-validation.json",
    "warmup_status": "warmup-status.json",
    "workload_validation": "workload-validation.json",
    "workload_status": "workload-status.json",
    "workload": "workload.txt",
    "pidstat": "tidb-pidstat.txt",
    "proc_after": "tidb-proc-after.txt",
    "timing": "timing.json",
    "checker_result": "checker-result.json",
    "post_check_validation": "post-check-validation.json",
}


TRIAL_COLUMNS = (
    "trial",
    "trial_index",
    "mode",
    "attempt",
    "attempt_path",
    "workload_label",
    "checker_metric_scope",
    "valid",
    "invalid_reason",
    "outcome",
    "verdict",
    "go_tpc_pass",
    "tpcc_post_check_pass",
    "tpm_c",
    "new_order_success_count",
    "new_order_transactions_per_second",
    "paired_baseline_tpm_c",
    "throughput_overhead_percent",
    "new_order_p99_latency_ms",
    "new_order_error_count",
    "new_order_error_rate_percent",
    "tidb_cpu_mean_percent",
    "tidb_max_rss_kib",
    "queue_depth_before_close",
    "queue_peak_depth",
    "queue_peak_bytes",
    "queue_backpressure_count",
    "queue_backpressure_time_ns",
    "queue_average_residence_time_ns",
    "queue_maximum_residence_time_ns",
    "captured_minus_ordered_transaction_tail",
    "captured_minus_checked_transaction_tail",
    "accepted_publications_minus_ordered_transaction_tail",
    "accepted_publications_minus_checked_transaction_tail",
    "accepted_events_minus_checked_events_tail",
    "global_lock_acquisition_count",
    "global_lock_average_wait_time_ns",
    "global_lock_maximum_wait_time_ns",
    "global_lock_average_hold_time_ns",
    "global_lock_maximum_hold_time_ns",
    "active_finalizers_at_close",
    "workload_start_monotonic_ns",
    "workload_client_exit_monotonic_ns",
    "checker_result_available_monotonic_ns",
    "tidb_server_exit_monotonic_ns",
    "server_shutdown_start_monotonic_ns",
    "actual_workload_duration_seconds",
    "workload_start_to_verdict_seconds",
    "post_workload_drain_to_verdict_seconds",
    "native_close_duration_seconds",
    "server_shutdown_duration_seconds",
    "checker_fatal",
    "checker_close_status",
    "captured_transactions",
    "captured_events",
    "accepted_publications",
    "accepted_events",
    "ordered_transactions",
    "checked_transactions",
    "checked_events",
    "publication_sequence",
    "successful_write_commits",
    "read_only_no_write_completions",
    "attempts_registered",
    "attempts_published",
    "attempts_aborted",
    "queue_depth_at_close",
    "queue_bytes_at_close",
    "queue_reserved_depth_at_close",
    "queue_reserved_bytes_at_close",
    "reorder_bytes_at_close",
)


SUMMARY_METRICS = (
    "tpm_c",
    "new_order_transactions_per_second",
    "throughput_overhead_percent",
    "new_order_p99_latency_ms",
    "new_order_error_rate_percent",
    "tidb_cpu_mean_percent",
    "tidb_max_rss_kib",
    "queue_depth_before_close",
    "queue_peak_depth",
    "queue_peak_bytes",
    "queue_backpressure_count",
    "queue_backpressure_time_ns",
    "queue_average_residence_time_ns",
    "queue_maximum_residence_time_ns",
    "captured_minus_ordered_transaction_tail",
    "captured_minus_checked_transaction_tail",
    "accepted_publications_minus_ordered_transaction_tail",
    "accepted_publications_minus_checked_transaction_tail",
    "global_lock_acquisition_count",
    "global_lock_average_wait_time_ns",
    "global_lock_maximum_wait_time_ns",
    "global_lock_average_hold_time_ns",
    "global_lock_maximum_hold_time_ns",
    "active_finalizers_at_close",
    "actual_workload_duration_seconds",
    "workload_start_to_verdict_seconds",
    "post_workload_drain_to_verdict_seconds",
    "native_close_duration_seconds",
    "server_shutdown_duration_seconds",
)


class AnalysisError(RuntimeError):
    """An experiment-level error that prevents construction of the matrix."""


def as_float(value: Any) -> float | None:
    if isinstance(value, bool) or value is None:
        return None
    try:
        number = float(str(value).rstrip("%"))
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def as_int(value: Any) -> int | None:
    if isinstance(value, bool) or value is None:
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value) if math.isfinite(value) and value.is_integer() else None
    text = str(value).strip()
    if re.fullmatch(r"[+-]?\d+", text):
        try:
            return int(text)
        except ValueError:
            return None
    number = as_float(value)
    return (
        int(number)
        if number is not None and number.is_integer()
        else None
    )


def first(record: Any, *keys: str) -> Any:
    if not isinstance(record, dict):
        return None
    for key in keys:
        if key in record:
            return record[key]
    return None


def first_int(record: Any, *keys: str) -> int | None:
    return as_int(first(record, *keys))


def first_float(record: Any, *keys: str) -> float | None:
    return as_float(first(record, *keys))


def add_reason(reasons: list[str], reason: str) -> None:
    if reason not in reasons:
        reasons.append(reason)


def load_json(
    path: pathlib.Path,
    reasons: list[str],
    label: str,
    *,
    required: bool = True,
) -> dict[str, Any] | None:
    try:
        text = path.read_text(errors="replace")
    except (FileNotFoundError, IsADirectoryError, OSError) as error:
        if required:
            add_reason(reasons, f"missing {label}: {error}")
        return None
    try:
        value = json.loads(text)
    except json.JSONDecodeError as error:
        add_reason(reasons, f"invalid {label}: {error}")
        return None
    if not isinstance(value, dict):
        add_reason(reasons, f"invalid {label}: expected a JSON object")
        return None
    return value


def evidence_passed(record: Any) -> bool:
    if not isinstance(record, dict):
        return False
    for key in ("passed", "valid", "ok", "success"):
        if record.get(key) is True:
            return True
        if record.get(key) is False:
            return False
    exit_status = as_int(first(record, "exit_status", "status_code", "code"))
    if exit_status is not None:
        return exit_status == 0
    status = first(record, "status", "outcome", "result")
    return (
        isinstance(status, str)
        and status.lower() in {"pass", "passed", "complete", "completed", "success", "ok"}
    )


def require_evidence(
    attempt_dir: pathlib.Path,
    filename: str,
    reasons: list[str],
    description: str,
) -> tuple[dict[str, Any] | None, bool]:
    record = load_json(attempt_dir / filename, reasons, filename)
    passed = evidence_passed(record)
    if record is not None and not passed:
        add_reason(reasons, f"{description} did not pass")
    return record, passed


def parse_workload(path: pathlib.Path, reasons: list[str]) -> dict[str, Any]:
    result: dict[str, Any] = {
        "finished_count": 0,
        "malformed_json_count": 0,
        "new_order_summary_count": 0,
        "final_tpm_count": 0,
        "tpm_c": None,
        "success_count": None,
        "error_count": 0,
        "p99_latency_ms": None,
        "summary_seconds": None,
    }
    try:
        text = path.read_text(errors="replace")
    except (FileNotFoundError, IsADirectoryError, OSError) as error:
        add_reason(reasons, f"missing workload.txt: {error}")
        return result

    result["finished_count"] = sum(
        line.strip() == "Finished" for line in text.splitlines()
    )
    summary: dict[str, Any] | None = None
    error_summary: dict[str, Any] | None = None
    final: dict[str, Any] | None = None
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped.startswith(("[{", "{")):
            continue
        try:
            decoded = json.loads(stripped)
        except json.JSONDecodeError:
            result["malformed_json_count"] += 1
            continue
        records = decoded if isinstance(decoded, list) else [decoded]
        for record in records:
            if not isinstance(record, dict):
                continue
            operation = str(record.get("Operation", ""))
            prefix = str(record.get("Prefix", "")).strip()
            if prefix == "[Summary]" and operation == "NEW_ORDER":
                summary = record
                result["new_order_summary_count"] += 1
            elif prefix == "[Summary]" and operation == "NEW_ORDER_ERR":
                error_summary = record
            elif "tpmC" in record:
                final = record
                result["final_tpm_count"] += 1

    if summary is not None:
        result["success_count"] = as_int(summary.get("Count"))
        result["p99_latency_ms"] = as_float(summary.get("99th(ms)"))
        result["summary_seconds"] = as_float(summary.get("Takes(s)"))
        if final is None:
            result["tpm_c"] = as_float(summary.get("TPM"))
    if error_summary is not None:
        result["error_count"] = as_int(error_summary.get("Count"))
    if final is not None:
        result["tpm_c"] = as_float(final.get("tpmC"))

    if result["finished_count"] != 1:
        add_reason(
            reasons,
            f"go-tpc workload Finished marker count is {result['finished_count']}, expected 1",
        )
    if result["malformed_json_count"] != 0:
        add_reason(
            reasons,
            f"workload.txt has {result['malformed_json_count']} malformed JSON record(s)",
        )
    if result["new_order_summary_count"] != 1:
        add_reason(
            reasons,
            "go-tpc workload does not contain exactly one NEW_ORDER summary",
        )
    if result["final_tpm_count"] != 1:
        add_reason(
            reasons,
            "go-tpc workload does not contain exactly one final tpmC record",
        )
    if result["tpm_c"] is None:
        add_reason(reasons, "go-tpc workload tpmC is missing")
    if result["success_count"] is None:
        add_reason(reasons, "go-tpc NEW_ORDER success count is missing")
    if result["p99_latency_ms"] is None:
        add_reason(reasons, "go-tpc NEW_ORDER p99 latency is missing")
    if result["error_count"] is None:
        add_reason(reasons, "go-tpc NEW_ORDER error count is missing")
    return result


def parse_pidstat(path: pathlib.Path) -> tuple[float | None, int | None]:
    try:
        text = path.read_text(errors="replace")
    except (FileNotFoundError, IsADirectoryError, OSError):
        return None, None

    header: list[str] | None = None
    cpu_values: list[float] = []
    rss_values: list[int] = []
    for raw_line in text.splitlines():
        stripped = raw_line.strip()
        if not stripped or stripped.startswith("Linux"):
            continue
        candidate = stripped[1:].split() if stripped.startswith("#") else stripped.split()
        if "PID" in candidate and ("%CPU" in candidate or "RSS" in candidate):
            header = candidate
            continue
        if header is None or stripped.startswith("#"):
            continue
        fields = stripped.split()
        if fields and fields[0] == "Average:":
            continue
        if (
            len(fields) == len(header) + 1
            and len(fields) > 1
            and fields[1] in {"AM", "PM"}
        ):
            fields[0:2] = [fields[0] + " " + fields[1]]
        if len(fields) > len(header) and header[-1] == "Command":
            fields[len(header) - 1 :] = [" ".join(fields[len(header) - 1 :])]
        if len(fields) != len(header):
            continue
        record = dict(zip(header, fields))
        if as_int(record.get("PID")) is None:
            continue
        cpu = as_float(record.get("%CPU"))
        rss = as_int(record.get("RSS"))
        if cpu is not None:
            cpu_values.append(cpu)
        if rss is not None:
            rss_values.append(rss)
    return (
        statistics.fmean(cpu_values) if cpu_values else None,
        max(rss_values) if rss_values else None,
    )


def parse_proc_max_rss(path: pathlib.Path) -> int | None:
    try:
        text = path.read_text(errors="replace")
    except (FileNotFoundError, IsADirectoryError, OSError):
        return None
    values: list[int] = []
    for label in ("VmHWM", "VmRSS"):
        match = re.search(rf"^{label}:\s*(\d+)\s+kB\s*$", text, re.MULTILINE)
        if match:
            values.append(int(match.group(1)))
    return max(values) if values else None


def require_int(
    record: Any,
    keys: tuple[str, ...],
    reasons: list[str],
    label: str,
) -> int | None:
    value = first_int(record, *keys)
    if value is None:
        add_reason(reasons, f"missing integer checker metric {label}")
    return value


def zero_frontier(record: Any) -> bool:
    if record is None:
        return True
    if not isinstance(record, dict):
        return False
    for value in record.values():
        if value in (None, False, "", 0, 0.0):
            continue
        if as_int(value) == 0:
            continue
        return False
    return True


def validate_checker(
    row: dict[str, Any],
    result: dict[str, Any],
    reasons: list[str],
    mode: str,
) -> None:
    pipeline_mode = PIPELINE_MODE[mode]
    expected_native_mode = NATIVE_MODE[mode]
    expected_outcome = EXPECTED_OUTCOME[mode]
    adapter = result.get("adapter_metrics")
    native = result.get("native_metrics")
    preclose = result.get("native_metrics_before_close")
    if not isinstance(adapter, dict):
        add_reason(reasons, "checker result adapter_metrics is missing")
        adapter = {}
    if not isinstance(native, dict):
        add_reason(reasons, "checker result native_metrics is missing")
        native = {}
    if not isinstance(preclose, dict):
        add_reason(reasons, "checker result native_metrics_before_close is missing")
        preclose = {}

    row["outcome"] = result.get("outcome")
    verdict = result.get("verdict")
    row["verdict"] = verdict.get("status") if isinstance(verdict, dict) else None
    row["checker_close_status"] = as_int(result.get("close_status"))
    row["checker_fatal"] = native.get("fatal")

    if result.get("format_version") != 1:
        add_reason(reasons, "unexpected checker result format_version")
    if result.get("mode") != pipeline_mode:
        add_reason(reasons, "checker result pipeline mode does not match")
    if result.get("ordering_protocol") != "serialized-global-lock":
        add_reason(reasons, "checker result ordering protocol is not serialized-global-lock")
    if result.get("outcome") != expected_outcome:
        add_reason(reasons, f"checker outcome is not {expected_outcome}")
    if row["checker_close_status"] != 0:
        add_reason(reasons, "checker close status is nonzero")
    if result.get("runtime_error") not in (None, {}):
        add_reason(reasons, "checker runtime_error is present")
    if result.get("close_error") not in (None, ""):
        add_reason(reasons, "checker close_error is present")
    if not zero_frontier(result.get("frontier_metrics")):
        add_reason(reasons, "frontier activity was recorded for serialized-global-lock")

    native_mode = require_int(native, ("mode",), reasons, "native mode")
    native_protocol = require_int(
        native, ("ordering_protocol",), reasons, "native ordering_protocol"
    )
    if native_mode is not None and native_mode != expected_native_mode:
        add_reason(reasons, "native checker mode does not match")
    if native_protocol is not None and native_protocol != 1:
        add_reason(reasons, "native ordering protocol is not serialized-global-lock")
    if native.get("closed") is not True:
        add_reason(reasons, "native checker is not closed")
    if native.get("fatal") is not False:
        add_reason(reasons, "native checker fatal flag is set")

    attempts_registered = require_int(
        adapter, ("attempts_registered",), reasons, "adapter attempts_registered"
    )
    attempts_published = require_int(
        adapter, ("attempts_published",), reasons, "adapter attempts_published"
    )
    attempts_aborted = require_int(
        adapter, ("attempts_aborted",), reasons, "adapter attempts_aborted"
    )
    accepted_adapter = require_int(
        adapter, ("accepted_publications",), reasons, "adapter accepted_publications"
    )
    successful_writes = require_int(
        adapter,
        ("successful_write_commits",),
        reasons,
        "adapter successful_write_commits",
    )
    completions = require_int(
        adapter,
        ("read_only_no_write_completions", "completion_transactions"),
        reasons,
        "adapter read_only_no_write_completions",
    )
    publication_sequence = require_int(
        adapter, ("publication_sequence",), reasons, "adapter publication_sequence"
    )
    active_finalizers = require_int(
        adapter, ("active_finalizers",), reasons, "adapter active_finalizers"
    )
    lock_acquisitions = require_int(
        adapter,
        ("finalization_acquisitions", "global_lock_acquisitions"),
        reasons,
        "adapter finalization_acquisitions",
    )
    lock_wait_total = require_int(
        adapter,
        ("finalization_wait_nanos", "global_lock_wait_nanos"),
        reasons,
        "adapter finalization_wait_nanos",
    )
    lock_wait_max = require_int(
        adapter,
        ("max_finalization_wait_nanos", "global_lock_max_wait_nanos"),
        reasons,
        "adapter max_finalization_wait_nanos",
    )
    lock_hold_total = require_int(
        adapter,
        ("finalization_hold_nanos", "global_lock_hold_nanos"),
        reasons,
        "adapter finalization_hold_nanos",
    )
    lock_hold_max = require_int(
        adapter,
        ("max_finalization_hold_nanos", "global_lock_max_hold_nanos"),
        reasons,
        "adapter max_finalization_hold_nanos",
    )

    row.update(
        {
            "attempts_registered": attempts_registered,
            "attempts_published": attempts_published,
            "attempts_aborted": attempts_aborted,
            "publication_sequence": publication_sequence,
            "successful_write_commits": successful_writes,
            "read_only_no_write_completions": completions,
            "global_lock_acquisition_count": lock_acquisitions,
            "global_lock_maximum_wait_time_ns": lock_wait_max,
            "global_lock_maximum_hold_time_ns": lock_hold_max,
            "active_finalizers_at_close": active_finalizers,
        }
    )
    if lock_acquisitions is not None and lock_acquisitions > 0:
        if lock_wait_total is not None:
            row["global_lock_average_wait_time_ns"] = (
                lock_wait_total / lock_acquisitions
            )
        if lock_hold_total is not None:
            row["global_lock_average_hold_time_ns"] = (
                lock_hold_total / lock_acquisitions
            )
    elif accepted_adapter not in (None, 0):
        add_reason(reasons, "global lock acquisition count is zero")
    if (
        lock_wait_total is not None
        and lock_wait_max is not None
        and lock_wait_max > lock_wait_total
    ):
        add_reason(reasons, "maximum global lock wait exceeds total wait")
    if (
        lock_hold_total is not None
        and lock_hold_max is not None
        and lock_hold_max > lock_hold_total
    ):
        add_reason(reasons, "maximum global lock hold exceeds total hold")
    if active_finalizers not in (None, 0):
        add_reason(reasons, "active finalizers remain at close")

    if (
        attempts_registered is not None
        and attempts_published is not None
        and attempts_aborted is not None
        and attempts_registered != attempts_published + attempts_aborted
    ):
        add_reason(reasons, "adapter attempt lifecycle counters do not balance")
    if (
        accepted_adapter is not None
        and attempts_published is not None
        and accepted_adapter != attempts_published
    ):
        add_reason(reasons, "adapter accepted publications do not equal published attempts")
    if (
        accepted_adapter is not None
        and successful_writes is not None
        and completions is not None
        and accepted_adapter != successful_writes + completions
    ):
        add_reason(reasons, "adapter completion/publication counters do not balance")
    if (
        accepted_adapter is not None
        and publication_sequence is not None
        and accepted_adapter != publication_sequence
    ):
        add_reason(reasons, "adapter publication sequence is not dense")
    for field in ("fatal_calls", "publish_errors", "abort_errors"):
        value = require_int(adapter, (field,), reasons, f"adapter {field}")
        if value not in (None, 0):
            add_reason(reasons, f"adapter {field} is nonzero")

    active_attempts = require_int(
        native, ("active_attempts",), reasons, "native active_attempts"
    )
    captured_transactions = require_int(
        native, ("captured_transactions",), reasons, "native captured_transactions"
    )
    captured_events = require_int(
        native, ("captured_events",), reasons, "native captured_events"
    )
    accepted_native = require_int(
        native, ("accepted_publications",), reasons, "native accepted_publications"
    )
    accepted_events = first_int(native, "accepted_events")
    ordered = require_int(
        native, ("ordered_transactions",), reasons, "native ordered_transactions"
    )
    checked = require_int(
        native, ("checked_transactions",), reasons, "native checked_transactions"
    )
    checked_events = require_int(
        native, ("checked_events",), reasons, "native checked_events"
    )
    recycled = require_int(
        native, ("recycled_transactions",), reasons, "native recycled_transactions"
    )
    committed_unpublished = require_int(
        native,
        ("committed_but_unpublished",),
        reasons,
        "native committed_but_unpublished",
    )
    row.update(
        {
            "captured_transactions": captured_transactions,
            "captured_events": captured_events,
            "accepted_publications": accepted_native,
            "accepted_events": accepted_events,
            "ordered_transactions": ordered,
            "checked_transactions": checked,
            "checked_events": checked_events,
        }
    )
    if active_attempts not in (None, 0):
        add_reason(reasons, "native active attempts remain at close")
    if committed_unpublished not in (None, 0):
        add_reason(reasons, "committed-but-unpublished transactions are nonzero")
    if (
        accepted_adapter is not None
        and accepted_native is not None
        and accepted_adapter != accepted_native
    ):
        add_reason(reasons, "adapter/native accepted publication counts differ")
    if mode == "serialized-capture-only":
        if (
            accepted_native is not None
            and recycled is not None
            and accepted_native != recycled
        ):
            add_reason(reasons, "capture-only accepted/recycled counters differ")
        if ordered not in (None, 0) or checked not in (None, 0):
            add_reason(reasons, "capture-only unexpectedly ordered or checked transactions")
    else:
        if (
            accepted_native is not None
            and ordered is not None
            and accepted_native != ordered
        ):
            add_reason(reasons, "accepted publications do not equal ordered transactions")
    if mode == "serialized-full":
        if accepted_events is None:
            add_reason(reasons, "missing integer checker metric native accepted_events")
        if (
            accepted_native is not None
            and checked is not None
            and accepted_native != checked
        ):
            add_reason(reasons, "accepted publications do not equal checked transactions")
        if (
            accepted_events is not None
            and checked_events is not None
            and accepted_events != checked_events
        ):
            add_reason(reasons, "accepted events do not equal checked events")
        violations = require_int(
            native,
            ("violating_transactions",),
            reasons,
            "native violating_transactions",
        )
        if violations not in (None, 0):
            add_reason(reasons, "full mode reports consistency violations")
        if row["verdict"] != "pass":
            add_reason(reasons, "full checker verdict is not pass")

    ownership_fields = {
        "queue_depth_at_close": ("queue_depth",),
        "queue_bytes_at_close": ("queue_bytes",),
        "queue_reserved_depth_at_close": ("queue_reserved_depth",),
        "queue_reserved_bytes_at_close": ("queue_reserved_bytes",),
        "reorder_bytes_at_close": ("reorder_bytes",),
    }
    for destination, keys in ownership_fields.items():
        value = require_int(native, keys, reasons, f"native {keys[0]}")
        row[destination] = value
        if value not in (None, 0):
            add_reason(reasons, f"{keys[0]} did not drain to zero")
    capture_active = require_int(
        native, ("capture_active_bytes",), reasons, "native capture_active_bytes"
    )
    if capture_active not in (None, 0):
        add_reason(reasons, "capture_active_bytes did not drain to zero")
    reorder_capacity = require_int(
        native,
        ("reorder_capacity_bytes",),
        reasons,
        "native reorder_capacity_bytes",
    )
    if reorder_capacity not in (None, 0):
        add_reason(reasons, "serialized reorder capacity is not zero")
    reorder_peak = require_int(
        native, ("reorder_peak_bytes",), reasons, "native reorder_peak_bytes"
    )
    if reorder_peak not in (None, 0):
        add_reason(reasons, "serialized reorder peak bytes are nonzero")

    pre_captured = require_int(
        preclose,
        ("captured_transactions",),
        reasons,
        "pre-close captured_transactions",
    )
    pre_accepted = require_int(
        preclose,
        ("accepted_publications",),
        reasons,
        "pre-close accepted_publications",
    )
    pre_ordered = require_int(
        preclose,
        ("ordered_transactions",),
        reasons,
        "pre-close ordered_transactions",
    )
    pre_checked = require_int(
        preclose,
        ("checked_transactions",),
        reasons,
        "pre-close checked_transactions",
    )
    row["queue_depth_before_close"] = require_int(
        preclose, ("queue_depth",), reasons, "pre-close queue_depth"
    )
    differences = (
        (
            "captured_minus_ordered_transaction_tail",
            pre_captured,
            pre_ordered,
        ),
        (
            "captured_minus_checked_transaction_tail",
            pre_captured,
            pre_checked,
        ),
        (
            "accepted_publications_minus_ordered_transaction_tail",
            pre_accepted,
            pre_ordered,
        ),
        (
            "accepted_publications_minus_checked_transaction_tail",
            pre_accepted,
            pre_checked,
        ),
    )
    for destination, total, completed in differences:
        if total is not None and completed is not None:
            row[destination] = total - completed
            if total < completed:
                add_reason(reasons, f"{destination} is negative")
    if (
        accepted_events is not None
        and checked_events is not None
    ):
        row["accepted_events_minus_checked_events_tail"] = (
            accepted_events - checked_events
        )
    if (
        pre_captured is not None
        and captured_transactions is not None
        and pre_captured != captured_transactions
    ):
        add_reason(reasons, "transactions were captured after the pre-close snapshot")
    if (
        pre_accepted is not None
        and accepted_native is not None
        and pre_accepted != accepted_native
    ):
        add_reason(reasons, "publications were accepted after the pre-close snapshot")

    row["queue_peak_depth"] = require_int(
        native, ("queue_peak_depth",), reasons, "native queue_peak_depth"
    )
    row["queue_peak_bytes"] = require_int(
        native, ("queue_peak_bytes",), reasons, "native queue_peak_bytes"
    )
    row["queue_backpressure_count"] = require_int(
        native,
        ("queue_backpressure_wait_count", "queue_backpressure_count"),
        reasons,
        "native queue_backpressure_wait_count",
    )
    row["queue_backpressure_time_ns"] = require_int(
        native,
        ("queue_backpressure_ns", "queue_backpressure_time_ns"),
        reasons,
        "native queue_backpressure_ns",
    )
    queue_total_lag = require_int(
        native,
        ("total_queue_lag_ns", "queue_total_lag_ns"),
        reasons,
        "native total_queue_lag_ns",
    )
    queue_max_lag = require_int(
        native,
        ("max_queue_lag_ns", "queue_max_residence_time_ns"),
        reasons,
        "native max_queue_lag_ns",
    )
    row["queue_maximum_residence_time_ns"] = queue_max_lag
    residence_samples = first_int(native, "queue_residence_sample_count")
    if residence_samples is None:
        residence_samples = accepted_native
    if (
        queue_total_lag is not None
        and residence_samples is not None
        and residence_samples > 0
    ):
        row["queue_average_residence_time_ns"] = (
            queue_total_lag / residence_samples
        )
    elif queue_total_lag == 0 and residence_samples == 0:
        row["queue_average_residence_time_ns"] = 0.0
    if (
        queue_total_lag is not None
        and queue_max_lag is not None
        and queue_max_lag > queue_total_lag
    ):
        add_reason(reasons, "maximum queue residence time exceeds total residence time")


def new_row(
    trial_index: int,
    mode: str,
    workload_label: str,
    attempt_dir: pathlib.Path | None,
    root: pathlib.Path,
) -> dict[str, Any]:
    row = {column: None for column in TRIAL_COLUMNS}
    row.update(
        {
            "trial": f"trial-{trial_index:03d}",
            "trial_index": trial_index,
            "mode": mode,
            "attempt": attempt_dir.name if attempt_dir is not None else None,
            "attempt_path": (
                attempt_dir.relative_to(root).as_posix()
                if attempt_dir is not None
                else None
            ),
            "workload_label": workload_label,
            "checker_metric_scope": (
                CHECKER_METRIC_SCOPE if mode in CHECKED_MODES else None
            ),
            "valid": False,
            "invalid_reason": "",
        }
    )
    return row


def parse_timing(
    row: dict[str, Any],
    timing: dict[str, Any] | None,
    checker_result: dict[str, Any] | None,
    reasons: list[str],
    checked: bool,
) -> None:
    timing = timing or {}
    start = first_int(
        timing,
        "workload_start_monotonic_ns",
        "workload_start_ns",
        "measured_workload_start_monotonic_ns",
    )
    client_exit = first_int(
        timing,
        "workload_client_exit_monotonic_ns",
        "workload_client_exit_ns",
    )
    server_exit = first_int(
        timing,
        "tidb_server_exit_monotonic_ns",
        "server_exit_monotonic_ns",
        "server_exit_ns",
    )
    shutdown_start = first_int(
        timing,
        "server_shutdown_start_monotonic_ns",
        "shutdown_start_monotonic_ns",
        "shutdown_start_ns",
    )
    verdict_available = (
        first_int(
            timing,
            "checker_result_available_monotonic_ns",
            "checker_result_available_ns",
            "result_available_monotonic_ns",
        )
        if checked
        else None
    )
    row.update(
        {
            "workload_start_monotonic_ns": start,
            "workload_client_exit_monotonic_ns": client_exit,
            "checker_result_available_monotonic_ns": verdict_available,
            "tidb_server_exit_monotonic_ns": server_exit,
            "server_shutdown_start_monotonic_ns": shutdown_start,
        }
    )
    for value, label in (
        (start, "workload start monotonic timestamp"),
        (client_exit, "workload client exit monotonic timestamp"),
        (server_exit, "TiDB server exit monotonic timestamp"),
        (shutdown_start, "server shutdown start monotonic timestamp"),
    ):
        if value is None:
            add_reason(reasons, f"missing {label}")
    if checked and verdict_available is None:
        add_reason(reasons, "missing checker result available monotonic timestamp")
    if start is not None and client_exit is not None:
        if client_exit < start:
            add_reason(reasons, "workload client exit precedes workload start")
        else:
            row["actual_workload_duration_seconds"] = (
                client_exit - start
            ) / 1_000_000_000
    if shutdown_start is not None and server_exit is not None:
        if server_exit < shutdown_start:
            add_reason(reasons, "TiDB server exit precedes shutdown start")
        else:
            row["server_shutdown_duration_seconds"] = (
                server_exit - shutdown_start
            ) / 1_000_000_000
    if checked and start is not None and client_exit is not None and verdict_available is not None:
        if verdict_available < client_exit:
            add_reason(reasons, "checker result became available before workload client exit")
        else:
            row["workload_start_to_verdict_seconds"] = (
                verdict_available - start
            ) / 1_000_000_000
            row["post_workload_drain_to_verdict_seconds"] = (
                verdict_available - client_exit
            ) / 1_000_000_000
        # The timestamp is when the polling watcher first observes the atomic
        # result-file rename. Scheduler latency can place that observation just
        # after wait(2) reports server exit even though TiDB published the file
        # during shutdown, so do not turn that conservative delay into an
        # invalid run.

    native_close_ns = (
        first_int(checker_result, "native_close_nanoseconds")
        if checked and checker_result is not None
        else None
    )
    if checked:
        if native_close_ns is None:
            native_close_ns = first_int(timing, "native_close_nanoseconds")
        if native_close_ns is None:
            add_reason(reasons, "missing native close duration")
        elif native_close_ns < 0:
            add_reason(reasons, "native close duration is negative")
        else:
            row["native_close_duration_seconds"] = (
                native_close_ns / 1_000_000_000
            )


def parse_attempt(
    root: pathlib.Path,
    attempt_dir: pathlib.Path,
    trial_index: int,
    mode: str,
    workload_label: str,
) -> dict[str, Any]:
    row = new_row(trial_index, mode, workload_label, attempt_dir, root)
    reasons: list[str] = []
    configuration = load_json(
        attempt_dir / ATTEMPT_ARTIFACTS["configuration"],
        reasons,
        ATTEMPT_ARTIFACTS["configuration"]
    )
    runner_status = load_json(
        attempt_dir / ATTEMPT_ARTIFACTS["runner_status"],
        reasons,
        ATTEMPT_ARTIFACTS["runner_status"]
    )
    completed = load_json(
        attempt_dir / ATTEMPT_ARTIFACTS["completed"],
        reasons,
        ATTEMPT_ARTIFACTS["completed"]
    )
    if configuration is not None and not configuration:
        add_reason(reasons, "configuration.json is empty")
    if runner_status is not None and not evidence_passed(runner_status):
        add_reason(reasons, "runner status is not complete")
    if completed is not None and not evidence_passed(completed):
        add_reason(reasons, "completed marker is not valid")

    stage_results: dict[str, bool] = {}
    for stage in ("prepare", "warmup", "workload"):
        _, validation_passed = require_evidence(
            attempt_dir,
            ATTEMPT_ARTIFACTS[f"{stage}_validation"],
            reasons,
            f"{stage} validation",
        )
        _, status_passed = require_evidence(
            attempt_dir,
            ATTEMPT_ARTIFACTS[f"{stage}_status"],
            reasons,
            f"{stage} process",
        )
        stage_results[stage] = validation_passed and status_passed

    workload = parse_workload(attempt_dir / ATTEMPT_ARTIFACTS["workload"], reasons)
    raw_workload_pass = (
        workload["finished_count"] == 1
        and workload["malformed_json_count"] == 0
        and workload["new_order_summary_count"] == 1
        and workload["final_tpm_count"] == 1
        and workload["tpm_c"] is not None
        and workload["success_count"] is not None
        and workload["p99_latency_ms"] is not None
        and workload["error_count"] is not None
    )
    runner_go_tpc = (
        runner_status.get("go_tpc_pass")
        if isinstance(runner_status, dict) and "go_tpc_pass" in runner_status
        else True
    )
    row["go_tpc_pass"] = bool(
        stage_results["workload"] and raw_workload_pass and runner_go_tpc is True
    )
    if not row["go_tpc_pass"]:
        add_reason(reasons, "go-tpc workload did not pass")

    row["tpm_c"] = workload["tpm_c"]
    row["new_order_success_count"] = workload["success_count"]
    row["new_order_p99_latency_ms"] = workload["p99_latency_ms"]
    row["new_order_error_count"] = workload["error_count"]
    if workload["success_count"] is not None and workload["error_count"] is not None:
        attempts = workload["success_count"] + workload["error_count"]
        row["new_order_error_rate_percent"] = (
            workload["error_count"] * 100.0 / attempts if attempts else 0.0
        )

    cpu_mean, pidstat_max_rss = parse_pidstat(attempt_dir / ATTEMPT_ARTIFACTS["pidstat"])
    proc_max_rss = parse_proc_max_rss(attempt_dir / ATTEMPT_ARTIFACTS["proc_after"])
    row["tidb_cpu_mean_percent"] = cpu_mean
    rss_values = [
        value for value in (pidstat_max_rss, proc_max_rss) if value is not None
    ]
    row["tidb_max_rss_kib"] = max(rss_values) if rss_values else None
    if cpu_mean is None:
        add_reason(reasons, "TiDB CPU samples are missing")
    if row["tidb_max_rss_kib"] is None:
        add_reason(reasons, "TiDB maximum RSS evidence is missing")

    timing = load_json(attempt_dir / ATTEMPT_ARTIFACTS["timing"],
        reasons,
        ATTEMPT_ARTIFACTS["timing"])
    checker_result: dict[str, Any] | None = None
    if mode in CHECKED_MODES:
        checker_result = load_json(
            attempt_dir / ATTEMPT_ARTIFACTS["checker_result"],
            reasons,
            ATTEMPT_ARTIFACTS["checker_result"]
        )
        if checker_result is not None:
            validate_checker(row, checker_result, reasons, mode)
    elif (attempt_dir / ATTEMPT_ARTIFACTS["checker_result"]).exists():
        add_reason(reasons, "baseline unexpectedly produced checker-result.json")
    parse_timing(
        row,
        timing,
        checker_result,
        reasons,
        checked=mode in CHECKED_MODES,
    )
    duration = as_float(row.get("actual_workload_duration_seconds"))
    if (
        duration is not None
        and duration > 0
        and workload["success_count"] is not None
    ):
        row["new_order_transactions_per_second"] = (
            workload["success_count"] / duration
        )
    else:
        add_reason(reasons, "actual workload duration is unavailable")

    post_check, post_check_passed = require_evidence(
        attempt_dir,
        ATTEMPT_ARTIFACTS["post_check_validation"],
        reasons,
        "TPC-C post-check",
    )
    row["tpcc_post_check_pass"] = post_check_passed
    if post_check is not None and not post_check_passed:
        add_reason(reasons, "TPC-C post-check did not pass")

    if not stage_results.get("prepare", False):
        add_reason(reasons, "TPC-C prepare phase did not pass")
    if not stage_results.get("warmup", False):
        add_reason(reasons, "TPC-C warmup phase did not pass")
    row["valid"] = not reasons
    row["invalid_reason"] = "; ".join(reasons)
    row["_completed_marker_valid"] = evidence_passed(completed)
    return row


def attempt_directories(mode_dir: pathlib.Path) -> list[pathlib.Path]:
    attempts: list[tuple[int, pathlib.Path]] = []
    try:
        children = list(mode_dir.iterdir())
    except (FileNotFoundError, NotADirectoryError, OSError):
        return []
    for child in children:
        match = ATTEMPT_PATTERN.fullmatch(child.name)
        if child.is_dir() and match:
            attempts.append((int(match.group(1)), child))
    return [path for _, path in sorted(attempts)]


def select_attempt(
    root: pathlib.Path,
    trial_index: int,
    mode: str,
    workload_label: str,
) -> dict[str, Any]:
    mode_dir = root / "trials" / f"trial-{trial_index:03d}" / mode
    attempts = attempt_directories(mode_dir)
    if not attempts:
        row = new_row(trial_index, mode, workload_label, None, root)
        row["invalid_reason"] = "no attempt directory found"
        return row
    parsed = [
        parse_attempt(root, path, trial_index, mode, workload_label)
        for path in attempts
    ]
    valid_completed = [
        row
        for row in parsed
        if row["valid"] and row.get("_completed_marker_valid") is True
    ]
    selected = valid_completed[-1] if valid_completed else parsed[-1]
    return selected


def clean_row(row: dict[str, Any]) -> dict[str, Any]:
    return {column: row.get(column) for column in TRIAL_COLUMNS}


def apply_paired_overhead(rows: list[dict[str, Any]]) -> None:
    by_trial: dict[int, dict[str, dict[str, Any]]] = {}
    for row in rows:
        by_trial.setdefault(int(row["trial_index"]), {})[str(row["mode"])] = row
    for modes in by_trial.values():
        baseline = modes["baseline-original"]
        baseline_tpm = as_float(baseline.get("tpm_c"))
        if baseline["valid"] and baseline_tpm is not None and baseline_tpm > 0:
            baseline["paired_baseline_tpm_c"] = baseline_tpm
            baseline["throughput_overhead_percent"] = 0.0
        for mode in CHECKED_MODES:
            checked = modes[mode]
            checked_tpm = as_float(checked.get("tpm_c"))
            if (
                baseline["valid"]
                and checked["valid"]
                and baseline_tpm is not None
                and baseline_tpm > 0
                and checked_tpm is not None
            ):
                checked["paired_baseline_tpm_c"] = baseline_tpm
                checked["throughput_overhead_percent"] = (
                    (baseline_tpm - checked_tpm) * 100.0 / baseline_tpm
                )


def numeric_distribution(values: Iterable[Any]) -> tuple[float | None, float | None, float | None]:
    numbers = [number for value in values if (number := as_float(value)) is not None]
    if not numbers:
        return None, None, None
    return statistics.median(numbers), min(numbers), max(numbers)


def summarize(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for mode in MODES:
        mode_rows = [row for row in rows if row["mode"] == mode]
        valid_rows = [row for row in mode_rows if row["valid"]]
        summary: dict[str, Any] = {
            "mode": mode,
            "valid_trial_count": len(valid_rows),
            "invalid_trial_count": len(mode_rows) - len(valid_rows),
        }
        for metric in SUMMARY_METRICS:
            median, minimum, maximum = numeric_distribution(
                row.get(metric) for row in valid_rows
            )
            summary[f"{metric}_median"] = median
            summary[f"{metric}_minimum"] = minimum
            summary[f"{metric}_maximum"] = maximum
        result.append(summary)
    return result


def csv_value(value: Any) -> Any:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    return value


def render_csv(rows: list[dict[str, Any]], columns: Iterable[str]) -> str:
    fieldnames = list(columns)
    output = io.StringIO(newline="")
    writer = csv.DictWriter(output, fieldnames=fieldnames, lineterminator="\n")
    writer.writeheader()
    for row in rows:
        writer.writerow({key: csv_value(row.get(key)) for key in fieldnames})
    return output.getvalue()


def summary_columns() -> list[str]:
    fields = ["mode", "valid_trial_count", "invalid_trial_count"]
    for metric in SUMMARY_METRICS:
        fields.extend(
            (
                f"{metric}_median",
                f"{metric}_minimum",
                f"{metric}_maximum",
            )
        )
    return fields


def format_number(value: Any, digits: int = 2, suffix: str = "") -> str:
    number = as_float(value)
    return "—" if number is None else f"{number:.{digits}f}{suffix}"


def markdown(summary: dict[str, Any]) -> str:
    lines = [
        "# TPC-C NewOrder-only serialized global-lock comparison",
        "",
        f"Result root: `{summary['result_root']}`",
        "",
        f"Overall validity: **{'PASS' if summary['all_valid'] else 'INVALID'}**",
        "",
        "## Per-trial results",
        "",
        "| Trial | Mode | Valid | tpmC | NewOrder/s | Overhead | p99 | Error | CPU | RSS | Queue peak d/B | Lock wait avg/max | Lock hold avg/max | Start-to-verdict | Drain | Native close | Shutdown |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in summary["trial_rows"]:
        lines.append(
            "| {trial} | {mode} | {valid} | {tpm} | {tps} | {overhead} | {p99} | {error} | {cpu} | {rss} | {queue} | {lock_wait} | {lock_hold} | {verdict} | {drain} | {close} | {shutdown} |".format(
                trial=row["trial"],
                mode=row["mode"],
                valid="yes" if row["valid"] else "no",
                tpm=format_number(row.get("tpm_c")),
                tps=format_number(row.get("new_order_transactions_per_second")),
                overhead=format_number(
                    row.get("throughput_overhead_percent"), 2, "%"
                ),
                p99=format_number(row.get("new_order_p99_latency_ms"), 2, " ms"),
                error=format_number(row.get("new_order_error_rate_percent"), 2, "%"),
                cpu=format_number(row.get("tidb_cpu_mean_percent"), 2, "%"),
                rss=format_number(row.get("tidb_max_rss_kib"), 0, " KiB"),
                queue=(f"{format_number(row.get('queue_peak_depth'), 0)}/"
                       f"{format_number(row.get('queue_peak_bytes'), 0)}"),
                lock_wait=(f"{format_number(row.get('global_lock_average_wait_time_ns'), 0)}/"
                           f"{format_number(row.get('global_lock_maximum_wait_time_ns'), 0)} ns"),
                lock_hold=(f"{format_number(row.get('global_lock_average_hold_time_ns'), 0)}/"
                           f"{format_number(row.get('global_lock_maximum_hold_time_ns'), 0)} ns"),
                verdict=format_number(
                    row.get("workload_start_to_verdict_seconds"), 3, " s"
                ),
                drain=format_number(
                    row.get("post_workload_drain_to_verdict_seconds"), 3, " s"
                ),
                close=format_number(row.get("native_close_duration_seconds"), 3, " s"),
                shutdown=format_number(row.get("server_shutdown_duration_seconds"), 3, " s"),
            )
        )

    lines.extend(
        [
            "",
            "## Valid-trial medians and ranges",
            "",
            "| Mode | Valid/invalid | tpmC | Overhead | p99 | Error | Queue peak | Lock wait avg | Lock hold avg | Start-to-verdict | Drain | Native close | Shutdown |",
            "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for row in summary["summary_rows"]:
        def ranged(metric: str, digits: int = 2, suffix: str = "") -> str:
            median = row.get(f"{metric}_median")
            minimum = row.get(f"{metric}_minimum")
            maximum = row.get(f"{metric}_maximum")
            if as_float(median) is None:
                return "—"
            return (
                f"{format_number(median, digits, suffix)} "
                f"({format_number(minimum, digits, suffix)}–"
                f"{format_number(maximum, digits, suffix)})"
            )

        lines.append(
            "| {mode} | {valid}/{invalid} | {tpm} | {overhead} | {p99} | {error} | {queue} | {lock_wait} | {lock_hold} | {verdict} | {drain} | {close} | {shutdown} |".format(
                mode=row["mode"],
                valid=row["valid_trial_count"],
                invalid=row["invalid_trial_count"],
                tpm=ranged("tpm_c"),
                overhead=ranged("throughput_overhead_percent", 2, "%"),
                p99=ranged("new_order_p99_latency_ms", 2, " ms"),
                error=ranged("new_order_error_rate_percent", 2, "%"),
                queue=ranged("queue_peak_depth", 0),
                lock_wait=ranged("global_lock_average_wait_time_ns", 0, " ns"),
                lock_hold=ranged("global_lock_average_hold_time_ns", 0, " ns"),
                drain=ranged("post_workload_drain_to_verdict_seconds", 3, " s"),
                close=ranged("native_close_duration_seconds", 3, " s"),
                shutdown=ranged("server_shutdown_duration_seconds", 3, " s"),
                verdict=ranged(
                    "workload_start_to_verdict_seconds", 3, " s"
                ),
            )
        )

    invalid = [row for row in summary["trial_rows"] if not row["valid"]]
    if invalid:
        lines.extend(["", "## Invalid runs", ""])
        for row in invalid:
            reason = str(row["invalid_reason"]).replace("\n", " ")
            lines.append(f"- `{row['trial']}/{row['mode']}`: {reason}")

    lines.extend(
        [
            "",
            "## Metric notes",
            "",
            "- Checked throughput overhead is paired only with a valid pristine baseline from the same trial.",
            "- `captured_minus_*` columns are literal native capture counters and may include prepared attempts that later aborted. `accepted_publications_minus_*` columns are the required serialized publication tails used for completeness.",
            "- Queue backpressure is reported but does not by itself invalidate a run; fatal timeout, lost evidence, or undrained ownership does.",
            f"- Checker metric scope: `{CHECKER_METRIC_SCOPE}`; queue and global-lock values are not workload-window-only metrics.",
            "- Baseline checker-verdict timing is intentionally blank. TPC-C post-check runs after TiDB exit and is excluded from all end-to-verdict calculations.",
            "",
        ]
    )
    return "\n".join(lines)


def atomic_write(path: pathlib.Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as stream:
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def analyze(root: pathlib.Path) -> dict[str, Any]:
    experiment_reasons: list[str] = []
    experiment = load_json(
        root / "experiment.json",
        experiment_reasons,
        "experiment.json",
    )
    if experiment is None:
        raise AnalysisError("; ".join(experiment_reasons))
    trials = as_int(experiment.get("trials"))
    if trials is None or trials <= 0:
        raise AnalysisError("experiment.json trials must be a positive integer")
    configured_modes = experiment.get("modes")
    if configured_modes is not None and configured_modes != list(MODES):
        raise AnalysisError("experiment.json modes do not match the fixed mode matrix")
    workload_label = str(
        experiment.get("workload_label", "TPC-C NewOrder-only")
    )

    rows = [
        select_attempt(root, trial, mode, workload_label)
        for trial in range(1, trials + 1)
        for mode in MODES
    ]
    apply_paired_overhead(rows)
    cleaned_rows = [clean_row(row) for row in rows]
    summary_rows = summarize(cleaned_rows)
    all_valid = (
        len(cleaned_rows) == trials * len(MODES)
        and all(row["valid"] for row in cleaned_rows)
    )
    return {
        "schema_version": 1,
        "result_root": root.as_posix(),
        "workload_label": workload_label,
        "expected_trials": trials,
        "modes": list(MODES),
        "all_valid": all_valid,
        "valid_run_count": sum(row["valid"] for row in cleaned_rows),
        "invalid_run_count": sum(not row["valid"] for row in cleaned_rows),
        "trial_rows": cleaned_rows,
        "summary_rows": summary_rows,
    }


def write_outputs(root: pathlib.Path, summary: dict[str, Any]) -> None:
    atomic_write(
        root / "tpcc-trials.csv",
        render_csv(summary["trial_rows"], TRIAL_COLUMNS),
    )
    atomic_write(
        root / "tpcc-summary.csv",
        render_csv(summary["summary_rows"], summary_columns()),
    )
    atomic_write(
        root / "tpcc-summary.json",
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
    )
    atomic_write(root / "tpcc-report.md", markdown(summary))


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Analyze pristine-vs-serialized-global-lock TPC-C result attempts."
        )
    )
    parser.add_argument("result_root", type=pathlib.Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    root = args.result_root.resolve()
    if not root.is_dir():
        print(f"result root is not a directory: {root}", file=sys.stderr)
        return 2
    try:
        summary = analyze(root)
        write_outputs(root, summary)
    except (AnalysisError, OSError, ValueError) as error:
        print(f"analysis failed: {error}", file=sys.stderr)
        return 2
    for name in (
        "tpcc-trials.csv",
        "tpcc-summary.csv",
        "tpcc-summary.json",
        "tpcc-report.md",
    ):
        print(f"wrote {root / name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
