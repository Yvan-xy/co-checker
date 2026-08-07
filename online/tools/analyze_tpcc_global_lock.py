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
BASELINE_FULL_MODES = (
    "baseline-original",
    "serialized-full",
)
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
CHECKER_METRIC_SCOPE = "complete data preparation + warm-up + measured workload"
DEFAULT_TRANSACTION_WEIGHTS = (45, 43, 4, 4, 4)
TRANSACTIONS = (
    ("new_order", "NEW_ORDER"),
    ("payment", "PAYMENT"),
    ("order_status", "ORDER_STATUS"),
    ("delivery", "DELIVERY"),
    ("stock_level", "STOCK_LEVEL"),
)
ATTEMPT_ARTIFACTS = {
    "configuration": "configuration.json",
    "runner_status": "runner-status.json",
    "completed": "completed.json",
    "prepare_validation": "prepare-validation.json",
    "prepare_status": "prepare-status.json",
    "schema_bootstrap_validation": "schema-bootstrap-validation.json",
    "schema_bootstrap_digest": "schema-bootstrap-digest.json",
    "prepare_manifest": "prepare-manifest.json",
    "post_prepare_digest": "post-prepare-digest.json",
    "prepare_reference_validation": "prepare-reference-validation.json",
    "preparation_drain": "preparation-drain.json",
    "warmup_validation": "warmup-validation.json",
    "warmup_status": "warmup-status.json",
    "warmup_drain": "warmup-drain.json",
    "workload_validation": "workload-validation.json",
    "workload_status": "workload-status.json",
    "workload_drain": "workload-drain.json",
    "workload": "workload.txt",
    "workload_checker_samples": "workload-checker-samples.jsonl",
    "workload_cutoff": "workload-cutoff.json",
    "pidstat": "tidb-pidstat.txt",
    "resource_samples": "tidb-resource-samples.txt",
    "proc_after": "tidb-proc-after.txt",
    "timing": "timing.json",
    "checker_result": "checker-result.json",
    "post_check_validation": "post-check-validation.json",
    "server_log_validation": "server-log-validation.json",
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
    "fixed_window_new_order_transactions_per_minute",
    "end_to_verdict_new_order_transactions_per_minute",
    "paired_baseline_tpm_c",
    "paired_baseline_fixed_window_new_order_transactions_per_minute",
    "fixed_window_throughput_overhead_percent",
    "end_to_verdict_throughput_overhead_percent",
    "throughput_overhead_percent",
    "continued_write_conflict_count",
    "new_order_p99_latency_ms",
    "new_order_error_count",
    "new_order_error_rate_percent",
    "payment_success_count",
    "payment_transactions_per_second",
    "payment_p99_latency_ms",
    "payment_error_count",
    "payment_error_rate_percent",
    "order_status_success_count",
    "order_status_transactions_per_second",
    "order_status_p99_latency_ms",
    "order_status_error_count",
    "order_status_error_rate_percent",
    "delivery_success_count",
    "delivery_transactions_per_second",
    "delivery_p99_latency_ms",
    "delivery_error_count",
    "delivery_error_rate_percent",
    "stock_level_success_count",
    "stock_level_transactions_per_second",
    "stock_level_p99_latency_ms",
    "stock_level_error_count",
    "stock_level_error_rate_percent",
    "prepare_manifest_sha256",
    "prepare_transaction_count",
    "prepare_logical_write_count",
    "prepare_schema_digest_sha256",
    "prepare_data_digest_sha256",
    "prepare_database_digest_sha256",
    "preparation_drain_pass",
    "warmup_drain_pass",
    "workload_drain_pass",
    "tidb_workload_cpu_seconds",
    "checker_sample_count",
    "checker_successful_sample_count",
    "checker_sample_coverage_percent",
    "checker_max_sample_gap_seconds",
    "live_progress_window_seconds",
    "live_stalled_window_count",
    "live_growing_window_max_consecutive",
    "live_sustainability_pass",
    "measured_accepted_publications",
    "measured_accepted_events",
    "measured_consumer_transactions",
    "measured_checked_transactions",
    "measured_checked_events",
    "checker_transaction_service_rate_per_second",
    "checker_event_service_rate_per_second",
    "workload_peak_transaction_backlog",
    "workload_peak_event_backlog",
    "workload_peak_queue_depth",
    "workload_peak_queue_bytes",
    "cutoff_residual_transactions",
    "cutoff_residual_events",
    "drained_transactions",
    "drained_events",
    "cutoff_observation_delay_seconds",
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
    "active_attempts_at_close",
    "active_finalizers_at_close",
    "active_fragments_at_close",
    "fragment_buffer_active_bytes_at_close",
    "fragment_retained_active_bytes_at_close",
    "fragments_started",
    "fragments_accepted",
    "fragments_discarded",
    "fragments_late_discarded",
    "workload_start_monotonic_ns",
    "workload_client_exit_monotonic_ns",
    "checker_result_available_monotonic_ns",
    "tidb_server_exit_monotonic_ns",
    "checker_drain_started_monotonic_ns",
    "checker_drain_complete_monotonic_ns",
    "checker_completion_monotonic_ns",
    "live_prefix_verdict_monotonic_ns",
    "server_shutdown_start_monotonic_ns",
    "actual_workload_duration_seconds",
    "configured_workload_duration_seconds",
    "end_to_verdict_duration_seconds",
    "workload_start_to_verdict_seconds",
    "post_workload_drain_to_verdict_seconds",
    "native_close_duration_seconds",
    "server_shutdown_duration_seconds",
    "checker_drain_duration_seconds",
    "cutoff_to_live_verdict_seconds",
    "checker_fatal",
    "checker_close_status",
    "captured_transactions",
    "captured_events",
    "captured_used_bytes",
    "captured_reserved_bytes",
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
    "fixed_window_new_order_transactions_per_minute",
    "end_to_verdict_new_order_transactions_per_minute",
    "fixed_window_throughput_overhead_percent",
    "end_to_verdict_throughput_overhead_percent",
    "throughput_overhead_percent",
    "new_order_p99_latency_ms",
    "new_order_error_rate_percent",
    "payment_transactions_per_second",
    "payment_p99_latency_ms",
    "payment_error_rate_percent",
    "order_status_transactions_per_second",
    "order_status_p99_latency_ms",
    "order_status_error_rate_percent",
    "delivery_transactions_per_second",
    "delivery_p99_latency_ms",
    "delivery_error_rate_percent",
    "stock_level_transactions_per_second",
    "stock_level_p99_latency_ms",
    "stock_level_error_rate_percent",
    "tidb_cpu_mean_percent",
    "tidb_workload_cpu_seconds",
    "tidb_max_rss_kib",
    "queue_depth_before_close",
    "queue_peak_depth",
    "queue_peak_bytes",
    "queue_backpressure_count",
    "queue_backpressure_time_ns",
    "queue_average_residence_time_ns",
    "queue_maximum_residence_time_ns",
    "continued_write_conflict_count",
    "captured_events",
    "captured_used_bytes",
    "captured_reserved_bytes",
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
    "checker_sample_count",
    "checker_sample_coverage_percent",
    "checker_max_sample_gap_seconds",
    "live_progress_window_seconds",
    "live_stalled_window_count",
    "live_growing_window_max_consecutive",
    "measured_accepted_publications",
    "measured_accepted_events",
    "measured_consumer_transactions",
    "measured_checked_transactions",
    "measured_checked_events",
    "checker_transaction_service_rate_per_second",
    "checker_event_service_rate_per_second",
    "workload_peak_transaction_backlog",
    "workload_peak_event_backlog",
    "workload_peak_queue_depth",
    "workload_peak_queue_bytes",
    "cutoff_residual_transactions",
    "cutoff_residual_events",
    "checker_drain_duration_seconds",
    "workload_start_to_verdict_seconds",
    "post_workload_drain_to_verdict_seconds",
    "native_close_duration_seconds",
    "server_shutdown_duration_seconds",
)


PERFORMANCE_COLUMNS = (
    "mode",
    "valid_trial_count",
    "invalid_trial_count",
    "end_to_verdict_new_order_transactions_per_minute_median",
    "end_to_verdict_throughput_overhead_percent_median",
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


def validate_live_drain(
    record: dict[str, Any] | None,
    reasons: list[str],
    mode: str,
    description: str,
) -> bool:
    before = len(reasons)
    if not isinstance(record, dict):
        add_reason(reasons, f"{description} is missing")
        return False
    if not evidence_passed(record) or record.get("drained") is not True:
        add_reason(reasons, f"{description} did not pass")
    if mode == "baseline-original":
        if record.get("checker") != "disabled":
            add_reason(reasons, f"{description} baseline checker is not disabled")
        return len(reasons) == before
    adapter = record.get("adapter_metrics")
    native = record.get("native_metrics")
    if not isinstance(adapter, dict) or not isinstance(native, dict):
        add_reason(reasons, f"{description} metrics are missing")
        return False
    for field in (
        "active_attempts", "active_finalizers", "active_fragments",
        "fragment_buffer_active_bytes", "fragment_retained_active_bytes",
    ):
        if as_int(adapter.get(field)) != 0:
            add_reason(reasons, f"{description} adapter {field} did not drain")
    for field in (
        "active_attempts", "queue_reserved_bytes", "queue_reserved_depth",
        "queue_bytes", "queue_depth", "reorder_bytes",
    ):
        if as_int(native.get(field)) != 0:
            add_reason(reasons, f"{description} native {field} did not drain")
    if record.get("live_scrape_success") is not True:
        add_reason(reasons, f"{description} live scrape did not succeed")
    if as_int(adapter.get("fatal_calls")) != 0:
        add_reason(reasons, f"{description} adapter fatal_calls is nonzero or missing")
    if as_int(adapter.get("fragment_capture_errors")) != 0:
        add_reason(reasons, f"{description} fragment_capture_errors is nonzero or missing")
    native_fatal = as_int(native.get("fatal"))
    if native_fatal != 0:
        add_reason(reasons, f"{description} native fatal is nonzero or missing")
    started = as_int(adapter.get("fragments_started"))
    accepted_fragments = as_int(adapter.get("fragments_accepted"))
    discarded_fragments = as_int(adapter.get("fragments_discarded"))
    if (
        started is None or started < 0
        or accepted_fragments is None
        or discarded_fragments is None
        or started != accepted_fragments + discarded_fragments
    ):
        add_reason(reasons, f"{description} fragment lifecycle did not advance and balance")
    registered = as_int(adapter.get("attempts_registered"))
    published = as_int(adapter.get("attempts_published"))
    aborted = as_int(adapter.get("attempts_aborted"))
    if (
        registered is None or registered <= 0
        or published is None or aborted is None
        or registered != published + aborted
    ):
        add_reason(reasons, f"{description} attempt lifecycle did not advance and balance")
    pipeline_mode = PIPELINE_MODE[mode]
    adapter_accepted = as_int(adapter.get("accepted_publications"))
    publication_sequence = as_int(adapter.get("publication_sequence"))
    successful_writes = as_int(adapter.get("successful_write_commits"))
    read_only = as_int(adapter.get("read_only_no_write_completions"))
    native_accepted = as_int(native.get("accepted_publications"))
    captured_transactions = as_int(native.get("captured_transactions"))
    captured_events = as_int(native.get("captured_events"))
    if (
        adapter_accepted is None or adapter_accepted <= 0
        or adapter_accepted != published
        or publication_sequence != adapter_accepted
        or successful_writes is None or read_only is None
        or successful_writes + read_only != adapter_accepted
        or native_accepted != adapter_accepted
    ):
        add_reason(reasons, f"{description} publication lifecycle did not advance and balance")
    if captured_transactions is None or captured_transactions <= 0:
        add_reason(reasons, f"{description} captured transaction lifecycle did not advance")
    if captured_events is None or captured_events <= 0:
        add_reason(reasons, f"{description} captured event lifecycle did not advance")
    if pipeline_mode in ("order-only", "full"):
        accepted = native_accepted
        ordered = as_int(native.get("ordered_transactions"))
        if accepted is None or accepted <= 0 or ordered != accepted:
            add_reason(reasons, f"{description} ordering lifecycle did not advance and balance")
    if pipeline_mode == "full":
        accepted = native_accepted
        checked = as_int(native.get("checked_transactions"))
        if accepted is None or accepted <= 0 or checked != accepted:
            add_reason(reasons, f"{description} checking lifecycle did not advance and balance")
        accepted_events = as_int(native.get("accepted_events"))
        checked_events = as_int(native.get("checked_events"))
        if (
            accepted_events is None
            or accepted_events <= 0
            or checked_events != accepted_events
        ):
            add_reason(
                reasons,
                f"{description} checking event lifecycle did not advance and balance",
            )
        if as_int(native.get("verdict_available")) != 1:
            add_reason(reasons, f"{description} live verdict is unavailable")
        if as_int(native.get("verdict_pass")) != 1:
            add_reason(reasons, f"{description} live verdict is not pass")
        if as_int(native.get("verdict_violation")) != 0:
            add_reason(reasons, f"{description} live verdict reports a violation")
    return len(reasons) == before


def parse_workload(
    path: pathlib.Path,
    reasons: list[str],
    transaction_weights: tuple[int, ...] = DEFAULT_TRANSACTION_WEIGHTS,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "finished_count": 0,
        "malformed_json_count": 0,
        "final_tpm_count": 0,
        "tpm_c": None,
        "transactions": {},
        "failure_markers": [],
        "error_policy_record_count": 0,
        "continued_write_conflict_count": None,
        "rejected_run_error_count": None,
    }
    try:
        text = path.read_text(errors="replace")
    except (FileNotFoundError, IsADirectoryError, OSError) as error:
        add_reason(reasons, f"missing workload.txt: {error}")
        return result

    lower = text.lower()
    result["failure_markers"] = [
        marker
        for marker in (
            "panic:", "fatal error", "execute run failed", "failed to execute workload"
        )
        if marker in lower
    ]
    result["finished_count"] = sum(
        line.strip() == "Finished" for line in text.splitlines()
    )
    summaries = {operation: [] for _, operation in TRANSACTIONS}
    error_summaries = {operation: [] for _, operation in TRANSACTIONS}
    final_records: list[dict[str, Any]] = []
    policy_records: list[dict[str, Any]] = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped.startswith(("[", "{")):
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
            if prefix == "[Summary]" and operation in summaries:
                summaries[operation].append(record)
            elif prefix == "[Summary]" and operation.endswith("_ERR"):
                base = operation[:-4]
                if base in error_summaries:
                    error_summaries[base].append(record)
            if record.get("record") == "go-tpc-run-error-policy":
                policy_records.append(record)
            if "tpmC" in record:
                final_records.append(record)

    result["final_tpm_count"] = len(final_records)
    if len(final_records) == 1:
        result["tpm_c"] = as_float(final_records[0].get("tpmC"))
    result["error_policy_record_count"] = len(policy_records)
    if len(policy_records) != 1:
        add_reason(
            reasons,
            "go-tpc workload does not contain exactly one strict error-policy record",
        )
    else:
        policy = policy_records[0]
        continued = policy.get("continued_count")
        rejected = policy.get("rejected_count")
        if (
            policy.get("format_version") != 1
            or policy.get("policy") != "continue-only-tidb-write-conflict"
            or policy.get("mysql_error_number") != 9007
        ):
            add_reason(reasons, "go-tpc strict error-policy identity is malformed")
        if (
            isinstance(continued, bool)
            or not isinstance(continued, int)
            or continued < 0
        ):
            add_reason(reasons, "go-tpc continued write-conflict count is malformed")
        else:
            result["continued_write_conflict_count"] = continued
        result["rejected_run_error_count"] = rejected
        if rejected != 0:
            add_reason(
                reasons,
                f"go-tpc strict policy rejected {rejected!r} run errors",
            )

    transactions: dict[str, dict[str, Any]] = {}
    for (prefix, operation), weight in zip(TRANSACTIONS, transaction_weights):
        records = summaries[operation]
        errors = error_summaries[operation]
        if weight == 0:
            if records:
                add_reason(
                    reasons,
                    f"go-tpc zero-weight {operation} unexpectedly has a summary",
                )
            if errors:
                add_reason(
                    reasons,
                    f"go-tpc zero-weight {operation} unexpectedly has an error summary",
                )
            transactions[prefix] = {
                "success_count": 0,
                "error_count": 0,
                "p99_latency_ms": None,
                "summary_seconds": None,
            }
            continue
        if len(records) != 1:
            add_reason(
                reasons,
                f"go-tpc workload does not contain exactly one {operation} summary",
            )
            continue
        summary = records[0]
        error_count = 0
        if len(errors) > 1:
            add_reason(reasons, f"go-tpc workload has duplicate {operation}_ERR summaries")
        elif errors:
            error_count = as_int(errors[0].get("Count"))
        transaction = {
            "success_count": as_int(summary.get("Count")),
            "error_count": error_count,
            "p99_latency_ms": as_float(summary.get("99th(ms)")),
            "summary_seconds": as_float(summary.get("Takes(s)")),
        }
        transactions[prefix] = transaction
        if transaction["success_count"] is None or transaction["success_count"] <= 0:
            add_reason(reasons, f"go-tpc {operation} success count is missing or zero")
        if transaction["p99_latency_ms"] is None:
            add_reason(reasons, f"go-tpc {operation} p99 latency is missing")
        if transaction["summary_seconds"] is None:
            add_reason(reasons, f"go-tpc {operation} summary duration is missing")
        if transaction["error_count"] is None:
            add_reason(reasons, f"go-tpc {operation} error count is malformed")
        elif transaction["error_count"] < 0:
            add_reason(
                reasons,
                f"go-tpc {operation} reports a negative error count",
            )
    result["transactions"] = transactions
    new_order = transactions.get("new_order", {})
    result["new_order_summary_count"] = len(summaries["NEW_ORDER"])
    result["success_count"] = new_order.get("success_count")
    result["error_count"] = new_order.get("error_count")
    reported_errors = [
        transaction.get("error_count") for transaction in transactions.values()
    ]
    if (
        result["continued_write_conflict_count"] is None
        or any(value is None for value in reported_errors)
        or sum(value for value in reported_errors if value is not None)
        != result["continued_write_conflict_count"]
    ):
        add_reason(
            reasons,
            "go-tpc transaction errors do not conserve classified Error 9007 conflicts",
        )
    result["p99_latency_ms"] = new_order.get("p99_latency_ms")
    result["summary_seconds"] = new_order.get("summary_seconds")

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
    if result["failure_markers"]:
        add_reason(reasons, f"go-tpc failure markers: {result['failure_markers']}")
    if result["final_tpm_count"] != 1:
        add_reason(reasons, "go-tpc workload does not contain exactly one final tpmC record")
    if result["tpm_c"] is None:
        add_reason(reasons, "go-tpc workload tpmC is missing")
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


def parse_resource_samples(
    path: pathlib.Path,
    start_ns: int | None,
    end_ns: int | None,
) -> tuple[float | None, int | None]:
    if start_ns is None or end_ns is None:
        return None, None
    try:
        lines = path.read_text(errors="replace").splitlines()
    except (FileNotFoundError, IsADirectoryError, OSError):
        return None, None
    if not lines:
        return None, None
    header = lines[0].split()
    required = {"monotonic_ns", "vmrss_kib", "process_cpu_ticks"}
    if not required.issubset(header):
        return None, None
    selected: list[tuple[int, int]] = []
    for line in lines[1:]:
        fields = line.split()
        if len(fields) != len(header):
            continue
        record = dict(zip(header, fields))
        timestamp = as_int(record.get("monotonic_ns"))
        rss = as_int(record.get("vmrss_kib"))
        ticks = as_int(record.get("process_cpu_ticks"))
        if (
            timestamp is not None
            and rss is not None
            and ticks is not None
            and start_ns <= timestamp <= end_ns
        ):
            selected.append((rss, ticks))
    if not selected:
        return None, None
    ticks_per_second = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
    cpu_seconds = (selected[-1][1] - selected[0][1]) / ticks_per_second
    return cpu_seconds, max(rss for rss, _ in selected)


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
    adapter_active_attempts = require_int(
        adapter, ("active_attempts",), reasons, "adapter active_attempts"
    )
    active_finalizers = require_int(
        adapter, ("active_finalizers",), reasons, "adapter active_finalizers"
    )
    active_fragments = require_int(
        adapter, ("active_fragments",), reasons, "adapter active_fragments"
    )
    fragment_buffer_active = require_int(
        adapter, ("fragment_buffer_active_bytes",), reasons,
        "adapter fragment_buffer_active_bytes",
    )
    fragment_retained_active = require_int(
        adapter, ("fragment_retained_active_bytes",), reasons,
        "adapter fragment_retained_active_bytes",
    )
    fragments_started = require_int(
        adapter, ("fragments_started",), reasons, "adapter fragments_started"
    )
    fragments_accepted = require_int(
        adapter, ("fragments_accepted",), reasons, "adapter fragments_accepted"
    )
    fragments_discarded = require_int(
        adapter, ("fragments_discarded",), reasons, "adapter fragments_discarded"
    )
    fragments_attached = require_int(
        adapter, ("fragments_storage_attached",), reasons,
        "adapter fragments_storage_attached",
    )
    fragments_late = require_int(
        adapter, ("fragments_late_discarded",), reasons,
        "adapter fragments_late_discarded",
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
            "active_attempts_at_close": adapter_active_attempts,
            "active_finalizers_at_close": active_finalizers,
            "active_fragments_at_close": active_fragments,
            "fragment_buffer_active_bytes_at_close": fragment_buffer_active,
            "fragment_retained_active_bytes_at_close": fragment_retained_active,
            "fragments_started": fragments_started,
            "fragments_accepted": fragments_accepted,
            "fragments_discarded": fragments_discarded,
            "fragments_late_discarded": fragments_late,
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
    for value, label in (
        (adapter_active_attempts, "adapter active attempts"),
        (active_finalizers, "active finalizers"),
        (active_fragments, "active fragments"),
        (fragment_buffer_active, "fragment buffer bytes"),
        (fragment_retained_active, "retained fragment bytes"),
    ):
        if value not in (None, 0):
            add_reason(reasons, f"{label} remain at close")
    if (
        fragments_started is not None
        and fragments_accepted is not None
        and fragments_discarded is not None
    ):
        if fragments_started <= 0:
            add_reason(reasons, "fragment lifecycle did not advance")
        if fragments_started != fragments_accepted + fragments_discarded:
            add_reason(reasons, "adapter fragment lifecycle counters do not balance")
    if (
        fragments_attached is not None
        and fragments_started is not None
        and fragments_attached > fragments_started
    ):
        add_reason(reasons, "storage-attached fragments exceed started fragments")
    for late_key, total_key in (
        ("fragments_late_discarded", "fragments_discarded"),
        ("fragment_late_discarded_events", "fragment_discarded_events"),
        ("fragment_late_discarded_bytes", "fragment_discarded_payload_bytes"),
    ):
        late_value = require_int(adapter, (late_key,), reasons, f"adapter {late_key}")
        total_value = require_int(adapter, (total_key,), reasons, f"adapter {total_key}")
        if (
            late_value is not None
            and total_value is not None
            and late_value > total_value
        ):
            add_reason(reasons, f"adapter {late_key} exceeds {total_key}")
    fragment_capture_errors = require_int(
        adapter, ("fragment_capture_errors",), reasons,
        "adapter fragment_capture_errors",
    )
    if fragment_capture_errors not in (None, 0):
        add_reason(reasons, "adapter fragment_capture_errors is nonzero")

    if attempts_registered is not None and attempts_registered <= 0:
        add_reason(reasons, "adapter attempt lifecycle did not advance")
    if accepted_adapter is not None and accepted_adapter <= 0:
        add_reason(reasons, "adapter publication lifecycle did not advance")
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
    captured_used_bytes = require_int(
        native, ("captured_used_bytes",), reasons, "native captured_used_bytes"
    )
    captured_reserved_bytes = require_int(
        native,
        ("captured_reserved_bytes",),
        reasons,
        "native captured_reserved_bytes",
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
            "captured_used_bytes": captured_used_bytes,
            "captured_reserved_bytes": captured_reserved_bytes,
            "accepted_publications": accepted_native,
            "accepted_events": accepted_events,
            "ordered_transactions": ordered,
            "checked_transactions": checked,
            "checked_events": checked_events,
        }
    )
    if active_attempts not in (None, 0):
        add_reason(reasons, "native active attempts remain at close")
    for value, label in (
        (captured_used_bytes, "native captured_used_bytes"),
        (captured_reserved_bytes, "native captured_reserved_bytes"),
    ):
        if value is not None and value < 0:
            add_reason(reasons, f"{label} is negative")
    if (
        captured_used_bytes is not None
        and captured_reserved_bytes is not None
        and captured_reserved_bytes < captured_used_bytes
    ):
        add_reason(
            reasons,
            "native captured_reserved_bytes is smaller than captured_used_bytes",
        )
    if captured_events is not None and captured_events > 0:
        if captured_used_bytes is not None and captured_used_bytes <= 0:
            add_reason(reasons, "native captured_used_bytes did not advance")
        if captured_reserved_bytes is not None and captured_reserved_bytes <= 0:
            add_reason(reasons, "native captured_reserved_bytes did not advance")
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


def load_checker_samples(
    path: pathlib.Path, reasons: list[str]
) -> list[dict[str, Any]]:
    if not path.is_file():
        add_reason(reasons, f"missing {path.name}")
        return []
    samples: list[dict[str, Any]] = []
    for line_number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        if not line.strip():
            continue
        try:
            sample = json.loads(line)
        except json.JSONDecodeError as error:
            add_reason(reasons, f"{path.name}:{line_number} is invalid JSON: {error}")
            continue
        if not isinstance(sample, dict) or sample.get("format_version") != 1:
            add_reason(reasons, f"{path.name}:{line_number} has invalid format")
            continue
        samples.append(sample)
    if not samples:
        add_reason(reasons, f"{path.name} contains no samples")
    return samples


def analyze_live_checker(
    row: dict[str, Any],
    samples_path: pathlib.Path,
    cutoff: dict[str, Any] | None,
    warmup_drain: dict[str, Any] | None,
    workload_drain: dict[str, Any] | None,
    reasons: list[str],
    mode: str,
    configured_duration: float | None,
) -> None:
    before = len(reasons)
    samples = load_checker_samples(samples_path, reasons)
    row["checker_sample_count"] = len(samples)
    start_ns = as_int(row.get("workload_start_monotonic_ns"))
    cutoff_ns = as_int(row.get("workload_client_exit_monotonic_ns"))
    if configured_duration is None or configured_duration <= 0:
        add_reason(reasons, "live checker analysis has no configured duration")
        row["live_sustainability_pass"] = False
        return
    if start_ns is None or cutoff_ns is None:
        add_reason(reasons, "live checker analysis has no workload timestamps")
        row["live_sustainability_pass"] = False
        return

    successful: list[dict[str, Any]] = []
    previous_timestamp: int | None = None
    for index, sample in enumerate(samples):
        timestamp = as_int(sample.get("monotonic_ns"))
        if timestamp is None:
            add_reason(reasons, f"checker sample {index} has no monotonic timestamp")
            continue
        if previous_timestamp is not None and timestamp <= previous_timestamp:
            add_reason(reasons, "checker sample timestamps are not strictly increasing")
        previous_timestamp = timestamp
        if sample.get("scrape_success") is True:
            successful.append(sample)
    row["checker_successful_sample_count"] = len(successful)

    one_second_ns = 1_000_000_000
    coverage_samples = [
        sample for sample in successful
        if (timestamp := as_int(sample.get("monotonic_ns"))) is not None
        and start_ns - one_second_ns <= timestamp <= cutoff_ns + one_second_ns
    ]
    expected_samples = max(1, math.floor(configured_duration) + 1)
    coverage = min(100.0, len(coverage_samples) * 100.0 / expected_samples)
    row["checker_sample_coverage_percent"] = coverage
    sample_timestamps = [
        as_int(sample.get("monotonic_ns")) for sample in coverage_samples
    ]
    sample_timestamps = [value for value in sample_timestamps if value is not None]
    maximum_gap = max(
        (
            (right - left) / 1_000_000_000
            for left, right in zip(sample_timestamps, sample_timestamps[1:])
        ),
        default=0.0,
    )
    row["checker_max_sample_gap_seconds"] = maximum_gap
    if coverage < 95.0:
        add_reason(reasons, f"checker sample coverage is {coverage:.3f}%, want at least 95%")
    if maximum_gap > 3.0:
        add_reason(reasons, f"checker sample gap is {maximum_gap:.3f}s, want at most 3s")

    if not isinstance(cutoff, dict):
        add_reason(reasons, "workload cutoff observation is missing")
        row["live_sustainability_pass"] = False
        return
    delay_ns = first_int(
        cutoff,
        "observation_delay_ns",
        "cutoff_observation_delay_ns",
    )
    if delay_ns is None or delay_ns < 0:
        add_reason(reasons, "workload cutoff observation delay is missing or negative")
    else:
        row["cutoff_observation_delay_seconds"] = delay_ns / 1_000_000_000
        if delay_ns > one_second_ns:
            add_reason(reasons, "workload cutoff observation delay exceeds 1 second")

    if mode == "baseline-original":
        for sample in samples:
            if sample.get("checker_enabled") is not False:
                add_reason(reasons, "baseline checker sample is not explicitly disabled")
            if sample.get("adapter") or sample.get("native") or sample.get("verdict") is not None:
                add_reason(reasons, "baseline sample unexpectedly contains checker metrics")
        if cutoff.get("checker") != "disabled" and cutoff.get("checker_enabled") is not False:
            add_reason(reasons, "baseline workload cutoff is not checker-disabled")
        row["live_sustainability_pass"] = len(reasons) == before
        return

    pipeline_mode = PIPELINE_MODE[mode]
    expected_identity = "serialized-global-lock"
    for sample in successful:
        if sample.get("checker_enabled") is not True:
            add_reason(reasons, "checked sample does not mark checker enabled")
        if sample.get("mode") != pipeline_mode:
            add_reason(reasons, "checked sample mode identity mismatch")
        if sample.get("ordering_protocol") != expected_identity:
            add_reason(reasons, "checked sample ordering identity mismatch")
        if not isinstance(sample.get("adapter"), dict) or not isinstance(sample.get("native"), dict):
            add_reason(reasons, "successful checked sample has missing metrics")

    if not isinstance(warmup_drain, dict) or not isinstance(workload_drain, dict):
        add_reason(reasons, "warmup/workload drain snapshots are missing")
        row["live_sustainability_pass"] = False
        return
    if workload_drain.get("format_version") != 2:
        add_reason(reasons, "workload drain format_version is not 2")
    start_native = warmup_drain.get("native_metrics")
    cutoff_native = cutoff.get("native_metrics")
    initial_native = workload_drain.get("initial_native_metrics")
    final_native = workload_drain.get("native_metrics")
    if not all(isinstance(value, dict) for value in (
        start_native, cutoff_native, initial_native, final_native
    )):
        add_reason(reasons, "live checker phase snapshots are incomplete")
        row["live_sustainability_pass"] = False
        return

    target_name = {
        "capture-only": "recycled_transactions",
        "order-only": "ordered_transactions",
        "full": "checked_transactions",
    }[pipeline_mode]
    start_accepted = as_int(start_native.get("accepted_publications"))
    cutoff_accepted = as_int(cutoff_native.get("accepted_publications"))
    initial_accepted = as_int(initial_native.get("accepted_publications"))
    final_accepted = as_int(final_native.get("accepted_publications"))
    start_target = as_int(start_native.get(target_name))
    cutoff_target = as_int(cutoff_native.get(target_name))
    initial_target = as_int(initial_native.get(target_name))
    final_target = as_int(final_native.get(target_name))
    counters = (
        start_accepted, cutoff_accepted, initial_accepted, final_accepted,
        start_target, cutoff_target, initial_target, final_target,
    )
    if any(value is None for value in counters):
        add_reason(reasons, "live checker transaction phase counters are missing")
        row["live_sustainability_pass"] = False
        return
    assert start_accepted is not None and cutoff_accepted is not None
    assert initial_accepted is not None and final_accepted is not None
    assert start_target is not None and cutoff_target is not None
    assert initial_target is not None and final_target is not None
    if not (start_accepted <= cutoff_accepted == initial_accepted == final_accepted):
        add_reason(reasons, "accepted publications changed across the workload cutoff barrier")
    if not (start_target <= cutoff_target == initial_target <= final_target):
        add_reason(reasons, "consumer transaction counters regress across phase snapshots")

    measured_accepted = cutoff_accepted - start_accepted
    measured_target = cutoff_target - start_target
    residual_transactions = cutoff_accepted - cutoff_target
    drained_transactions = final_target - cutoff_target
    row["measured_accepted_publications"] = measured_accepted
    row["measured_consumer_transactions"] = measured_target
    row["checker_transaction_service_rate_per_second"] = (
        measured_target / configured_duration
    )
    row["cutoff_residual_transactions"] = residual_transactions
    row["drained_transactions"] = drained_transactions
    if pipeline_mode == "full":
        row["measured_checked_transactions"] = measured_target
    if residual_transactions < 0 or drained_transactions != residual_transactions:
        add_reason(reasons, "cutoff transaction residual does not exactly drain")
    if first_int(
        cutoff,
        "residual_transactions",
        "cutoff_residual_transactions",
    ) != residual_transactions:
        add_reason(reasons, "workload cutoff transaction residual is inconsistent")
    if as_int(workload_drain.get("drained_transactions")) != drained_transactions:
        add_reason(reasons, "workload drained transaction count is inconsistent")
    allowed_tail = max(1, math.floor(max(0, measured_accepted) * 0.01))
    tail_within_target = residual_transactions <= allowed_tail

    start_events = as_int(start_native.get("accepted_events"))
    cutoff_events = as_int(cutoff_native.get("accepted_events"))
    final_events = as_int(final_native.get("accepted_events"))
    if start_events is not None and cutoff_events is not None and final_events is not None:
        row["measured_accepted_events"] = cutoff_events - start_events
    if pipeline_mode == "full":
        start_checked_events = as_int(start_native.get("checked_events"))
        cutoff_checked_events = as_int(cutoff_native.get("checked_events"))
        final_checked_events = as_int(final_native.get("checked_events"))
        if any(value is None for value in (
            start_events, cutoff_events, final_events,
            start_checked_events, cutoff_checked_events, final_checked_events,
        )):
            add_reason(reasons, "live checker event phase counters are missing")
        else:
            assert start_events is not None and cutoff_events is not None and final_events is not None
            assert start_checked_events is not None and cutoff_checked_events is not None
            assert final_checked_events is not None
            measured_checked_events = cutoff_checked_events - start_checked_events
            residual_events = cutoff_events - cutoff_checked_events
            drained_events = final_checked_events - cutoff_checked_events
            row["measured_checked_events"] = measured_checked_events
            row["checker_event_service_rate_per_second"] = (
                measured_checked_events / configured_duration
            )
            row["cutoff_residual_events"] = residual_events
            row["drained_events"] = drained_events
            if cutoff_events != final_events or residual_events < 0 or drained_events != residual_events:
                add_reason(reasons, "cutoff event residual does not exactly drain")
            if first_int(
                cutoff,
                "residual_events",
                "cutoff_residual_events",
            ) != residual_events:
                add_reason(reasons, "workload cutoff event residual is inconsistent")
            if as_int(workload_drain.get("drained_events")) != drained_events:
                add_reason(reasons, "workload drained event count is inconsistent")

    drain_seconds = as_float(row.get("checker_drain_duration_seconds"))
    drain_within_target = False
    if drain_seconds is None or drain_seconds < 0:
        add_reason(reasons, "checker drain duration is unavailable")
    else:
        drain_within_target = drain_seconds <= 30.0

    checked_samples = [
        sample for sample in coverage_samples
        if isinstance(sample.get("native"), dict)
    ]
    previous: dict[str, int] = {}
    points: list[tuple[int, int, int]] = []
    transaction_backlogs: list[int] = []
    event_backlogs: list[int] = []
    queue_depths: list[int] = []
    queue_bytes_values: list[int] = []
    for sample in checked_samples:
        native = sample["native"]
        timestamp = as_int(sample.get("monotonic_ns"))
        accepted = as_int(native.get("accepted_publications"))
        target = as_int(native.get(target_name))
        ordered = as_int(native.get("ordered_transactions")) or 0
        checked = as_int(native.get("checked_transactions")) or 0
        if timestamp is None or accepted is None or target is None:
            continue
        for name, value in (("accepted_publications", accepted), (target_name, target)):
            if name in previous and value < previous[name]:
                add_reason(reasons, f"live checker counter {name} decreased")
            previous[name] = value
        watermark = max(accepted, ordered, checked)
        backlog = max(0, watermark - target)
        points.append((timestamp, watermark, target))
        transaction_backlogs.append(backlog)
        queue_depth = as_int(native.get("queue_depth"))
        queue_bytes = as_int(native.get("queue_bytes"))
        if queue_depth is not None:
            queue_depths.append(queue_depth)
        if queue_bytes is not None:
            queue_bytes_values.append(queue_bytes)
        if pipeline_mode == "full":
            accepted_sample_events = as_int(native.get("accepted_events"))
            checked_sample_events = as_int(native.get("checked_events"))
            if accepted_sample_events is not None and checked_sample_events is not None:
                event_backlogs.append(max(0, accepted_sample_events - checked_sample_events))
    row["workload_peak_transaction_backlog"] = max(transaction_backlogs, default=0)
    row["workload_peak_event_backlog"] = max(event_backlogs, default=0)
    row["workload_peak_queue_depth"] = max(queue_depths, default=0)
    row["workload_peak_queue_bytes"] = max(queue_bytes_values, default=0)

    progress_window = (
        30.0
        if configured_duration >= 120.0
        else min(30.0, max(2.0, configured_duration / 4.0))
    )
    row["live_progress_window_seconds"] = progress_window
    window_ns = int(progress_window * 1_000_000_000)
    stalled = 0
    growing_consecutive = 0
    growing_max = 0
    window_start = start_ns
    while window_start + window_ns <= cutoff_ns:
        window_end = window_start + window_ns
        window_points = [point for point in points if window_start <= point[0] <= window_end]
        if len(window_points) >= 2:
            first_point, last_point = window_points[0], window_points[-1]
            publication_delta = last_point[1] - first_point[1]
            target_delta = last_point[2] - first_point[2]
            if publication_delta > 0 and target_delta <= 0:
                stalled += 1
            first_backlog = max(0, first_point[1] - first_point[2])
            last_backlog = max(0, last_point[1] - last_point[2])
            if publication_delta > 0 and last_backlog > first_backlog:
                growing_consecutive += 1
                growing_max = max(growing_max, growing_consecutive)
            else:
                growing_consecutive = 0
        window_start = window_end
    row["live_stalled_window_count"] = stalled
    row["live_growing_window_max_consecutive"] = growing_max
    if measured_accepted > 0 and measured_target <= 0:
        add_reason(reasons, "consumer did not advance during measured workload")
    if stalled:
        add_reason(reasons, f"checker consumer stalled in {stalled} active progress windows")
    # Tail size, a drain longer than 30 seconds, and a growing live backlog are
    # performance observations, not correctness failures. Exact residual
    # conservation and final zero ownership above remain fail-closed. Charging
    # the complete drain to end-to-verdict throughput makes slow consumers
    # visible without discarding their otherwise valid measurements.
    row["live_sustainability_pass"] = (
        len(reasons) == before
        and tail_within_target
        and drain_within_target
        and stalled == 0
        and growing_max < 3
    )


def parse_timing(
    row: dict[str, Any],
    timing: dict[str, Any] | None,
    checker_result: dict[str, Any] | None,
    reasons: list[str],
    checked: bool,
    mode: str,
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
    drain_started = first_int(
        timing, "checker_drain_started_monotonic_ns", "checker_drain_started_ns"
    ) if checked else None
    drain_complete = first_int(
        timing, "checker_drain_complete_monotonic_ns", "checker_drain_complete_ns"
    ) if checked else None
    checker_completion = first_int(
        timing, "checker_completion_monotonic_ns", "checker_completion_ns"
    ) if checked else None
    live_prefix_verdict = first_int(
        timing, "live_prefix_verdict_monotonic_ns", "live_prefix_verdict_ns"
    ) if checked else None
    # A full-mode prefix verdict can become available while accepted work still
    # remains queued. It is a validity signal only. Throughput completion is the
    # later all-work-complete timestamp for every checked mode.
    verdict_available = checker_completion
    row.update(
        {
            "workload_start_monotonic_ns": start,
            "workload_client_exit_monotonic_ns": client_exit,
            "checker_result_available_monotonic_ns": verdict_available,
            "checker_drain_started_monotonic_ns": drain_started,
            "checker_drain_complete_monotonic_ns": drain_complete,
            "checker_completion_monotonic_ns": checker_completion,
            "live_prefix_verdict_monotonic_ns": live_prefix_verdict,
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
        add_reason(reasons, "missing pre-close checker completion timestamp")
    if checked and drain_started is None:
        add_reason(reasons, "missing checker drain start monotonic timestamp")
    if checked and drain_complete is None:
        add_reason(reasons, "missing checker drain completion monotonic timestamp")
    if checked and mode == "serialized-full" and live_prefix_verdict is None:
        add_reason(reasons, "missing live prefix verdict monotonic timestamp")
    if (
        checked
        and mode == "serialized-full"
        and live_prefix_verdict is not None
        and checker_completion is not None
        and live_prefix_verdict > checker_completion
    ):
        add_reason(reasons, "live prefix verdict follows checker completion")
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
            add_reason(reasons, "checker completion precedes workload client exit")
        else:
            row["workload_start_to_verdict_seconds"] = (
                verdict_available - start
            ) / 1_000_000_000
            row["post_workload_drain_to_verdict_seconds"] = (
                verdict_available - client_exit
            ) / 1_000_000_000
            row["cutoff_to_live_verdict_seconds"] = (
                verdict_available - client_exit
            ) / 1_000_000_000
        if shutdown_start is not None and verdict_available > shutdown_start:
            add_reason(
                reasons,
                "checker completion was not observed before TiDB shutdown",
            )
    if checked and drain_started is not None and client_exit is not None:
        if drain_started < client_exit:
            add_reason(reasons, "checker drain starts before workload client exit")
    if checked and drain_started is not None and drain_complete is not None:
        if drain_complete < drain_started:
            add_reason(reasons, "checker drain completion precedes drain start")
        else:
            row["checker_drain_duration_seconds"] = (
                drain_complete - drain_started
            ) / 1_000_000_000
    if checked and drain_complete is not None and verdict_available is not None:
        if verdict_available != drain_complete:
            add_reason(
                reasons,
                "checker completion does not equal successful live drain completion",
            )

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
    expected_transaction_weights: tuple[int, ...] = DEFAULT_TRANSACTION_WEIGHTS,
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
    if isinstance(configuration, dict):
        workload_configuration = configuration.get("workload", {})
        preparation_configuration = configuration.get("preparation", {})
        storage_configuration = configuration.get("storage_configuration", {})
        if configuration.get("format_version") != 2:
            add_reason(reasons, "configuration format_version is not 2")
        if configuration.get("workload_label") != "TPC-C full mix":
            add_reason(reasons, "configuration workload label is not full mix")
        if workload_configuration.get("weights") != list(expected_transaction_weights):
            add_reason(
                reasons,
                "configuration transaction weights do not match experiment.json",
            )
        if workload_configuration.get("prepare_threads") != 1:
            add_reason(reasons, "configuration preparation worker count is not 1")
        if workload_configuration.get("go_tpc_ignore_error") is not False:
            add_reason(reasons, "configuration enables generic go-tpc ignore-error")
        if workload_configuration.get("go_tpc_silence_errors") is not True:
            add_reason(reasons, "configuration does not suppress per-error logging")
        if workload_configuration.get("go_tpc_error_policy") != {
            "policy": "continue-only-tidb-write-conflict",
            "mysql_error_numbers": [9007],
            "generic_ignore_error": False,
        }:
            add_reason(reasons, "configuration strict go-tpc error policy is malformed")
        if preparation_configuration.get("partition_type") != {"code": 1, "name": "HASH"}:
            add_reason(reasons, "configuration preparation partition type is not HASH (code 1)")
        if preparation_configuration.get("partitions") != 1:
            add_reason(reasons, "configuration preparation partition count is not 1")
        if preparation_configuration.get("use_foreign_keys") is not False:
            add_reason(reasons, "configuration preparation enables foreign keys")
        if storage_configuration.get("fresh_store") is not True:
            add_reason(reasons, "configuration does not require a fresh per-mode store")
        if storage_configuration.get("coprocessor_cache_capacity_mb") != 0:
            add_reason(reasons, "coprocessor cache is not disabled")
        if storage_configuration.get("tidb_store_batch_size") != 0:
            add_reason(reasons, "store batching is not disabled")
        if storage_configuration.get("tidb_allow_batch_cop") is not False:
            add_reason(reasons, "BatchCop is not disabled")
        if storage_configuration.get("tidb_index_lookup_pushdown_policy") != "force":
            add_reason(reasons, "index-lookup pushdown policy is not force")

    _, schema_bootstrap_passed = require_evidence(
        attempt_dir,
        ATTEMPT_ARTIFACTS["schema_bootstrap_validation"],
        reasons,
        "schema-only bootstrap",
    )
    preparation_validation, preparation_passed = require_evidence(
        attempt_dir,
        ATTEMPT_ARTIFACTS["prepare_reference_validation"],
        reasons,
        "deterministic preparation reference",
    )
    load_json(
        attempt_dir / ATTEMPT_ARTIFACTS["prepare_manifest"],
        reasons,
        ATTEMPT_ARTIFACTS["prepare_manifest"],
    )
    load_json(
        attempt_dir / ATTEMPT_ARTIFACTS["schema_bootstrap_digest"],
        reasons,
        ATTEMPT_ARTIFACTS["schema_bootstrap_digest"],
    )
    load_json(
        attempt_dir / ATTEMPT_ARTIFACTS["post_prepare_digest"],
        reasons,
        ATTEMPT_ARTIFACTS["post_prepare_digest"],
    )
    if isinstance(preparation_validation, dict):
        observed = preparation_validation.get("observed")
        reference = preparation_validation.get("reference")
        if not isinstance(observed, dict) or observed != reference:
            add_reason(reasons, "preparation observation does not exactly match baseline reference")
            observed = {}
        row.update({
            "prepare_manifest_sha256": observed.get("manifest_file_sha256"),
            "prepare_transaction_count": observed.get("transaction_count"),
            "prepare_logical_write_count": observed.get("logical_write_count"),
            "prepare_schema_digest_sha256": observed.get("schema_digest_sha256"),
            "prepare_data_digest_sha256": observed.get("data_digest_sha256"),
            "prepare_database_digest_sha256": observed.get("database_digest_sha256"),
        })

    drain_records: dict[str, dict[str, Any]] = {}
    for key, column, description in (
        ("preparation_drain", "preparation_drain_pass", "post-prepare live drain"),
        ("warmup_drain", "warmup_drain_pass", "post-warmup live drain"),
        ("workload_drain", "workload_drain_pass", "post-workload live drain"),
    ):
        drain_record = load_json(
            attempt_dir / ATTEMPT_ARTIFACTS[key], reasons, ATTEMPT_ARTIFACTS[key]
        )
        if isinstance(drain_record, dict):
            drain_records[key] = drain_record
        row[column] = validate_live_drain(
            drain_record, reasons, mode, description
        )

    stage_results: dict[str, bool] = {}
    stage_validations: dict[str, dict[str, Any]] = {}
    for stage in ("prepare", "warmup", "workload"):
        validation, validation_passed = require_evidence(
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

        if isinstance(validation, dict):
            stage_validations[stage] = validation
    workload = parse_workload(
        attempt_dir / ATTEMPT_ARTIFACTS["workload"],
        reasons,
        expected_transaction_weights,
    )
    raw_workload_pass = (
        workload["finished_count"] == 1
        and workload["malformed_json_count"] == 0
        and workload["final_tpm_count"] == 1
        and workload["tpm_c"] is not None
        and not workload["failure_markers"]
        and len(workload["transactions"]) == len(TRANSACTIONS)
        and workload["error_policy_record_count"] == 1
        and workload["rejected_run_error_count"] == 0
        and all(
            transaction.get("success_count", -1) >= 0
            and (weight == 0 or transaction.get("success_count", 0) > 0)
            and transaction.get("error_count", -1) >= 0
            and (weight == 0 or transaction.get("p99_latency_ms") is not None)
            for weight, (prefix, _) in zip(
                expected_transaction_weights,
                TRANSACTIONS,
            )
            for transaction in (workload["transactions"].get(prefix, {}),)
        )
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
    row["continued_write_conflict_count"] = workload["continued_write_conflict_count"]
    for prefix, _ in TRANSACTIONS:
        transaction = workload["transactions"].get(prefix, {})
        success = transaction.get("success_count")
        errors = transaction.get("error_count")
        row[f"{prefix}_success_count"] = success
        row[f"{prefix}_p99_latency_ms"] = transaction.get("p99_latency_ms")
        row[f"{prefix}_error_count"] = errors
        if success is not None and errors is not None:
            attempts = success + errors
            row[f"{prefix}_error_rate_percent"] = (
                errors * 100.0 / attempts if attempts else 0.0
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
        mode=mode,
    )
    resource_cpu_seconds, resource_max_rss = parse_resource_samples(
        attempt_dir / ATTEMPT_ARTIFACTS["resource_samples"],
        as_int(row.get("workload_start_monotonic_ns")),
        as_int(row.get("workload_client_exit_monotonic_ns")),
    )
    row["tidb_workload_cpu_seconds"] = resource_cpu_seconds
    if resource_max_rss is not None:
        row["tidb_max_rss_kib"] = resource_max_rss
    if resource_cpu_seconds is None:
        add_reason(reasons, "measured-window TiDB CPU tick samples are missing")
    if resource_max_rss is None:
        add_reason(reasons, "measured-window TiDB instantaneous RSS samples are missing")
    configured_duration = as_float(
        stage_validations.get("workload", {}).get("expected_seconds")
    )
    row["configured_workload_duration_seconds"] = configured_duration
    cutoff_record = load_json(
        attempt_dir / ATTEMPT_ARTIFACTS["workload_cutoff"],
        reasons,
        ATTEMPT_ARTIFACTS["workload_cutoff"],
    )
    analyze_live_checker(
        row,
        attempt_dir / ATTEMPT_ARTIFACTS["workload_checker_samples"],
        cutoff_record,
        drain_records.get("warmup_drain"),
        drain_records.get("workload_drain"),
        reasons,
        mode,
        configured_duration,
    )
    actual_duration = as_float(row.get("actual_workload_duration_seconds"))
    if actual_duration is not None and actual_duration > 0:
        for prefix, _ in TRANSACTIONS:
            success = workload["transactions"].get(prefix, {}).get("success_count")
            if success is not None:
                row[f"{prefix}_transactions_per_second"] = success / actual_duration
        new_order_success = (
            workload["transactions"].get("new_order", {}).get("success_count")
        )
        if new_order_success is not None:
            row[
                "fixed_window_new_order_transactions_per_minute"
            ] = new_order_success * 60.0 / actual_duration
            if mode in CHECKED_MODES:
                verdict_duration = as_float(
                    row.get("workload_start_to_verdict_seconds")
                )
                if verdict_duration is not None and verdict_duration > 0:
                    row["end_to_verdict_duration_seconds"] = verdict_duration
                    row[
                        "end_to_verdict_new_order_transactions_per_minute"
                    ] = new_order_success * 60.0 / verdict_duration
                else:
                    add_reason(
                        reasons,
                        "workload-start-to-checker-completion duration is unavailable",
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

    server_logs, server_logs_passed = require_evidence(
        attempt_dir,
        ATTEMPT_ARTIFACTS["server_log_validation"],
        reasons,
        "TiDB server logs",
    )
    if isinstance(server_logs, dict):
        expected_server_logs = {
            "schema-bootstrap-server.txt",
            "schema-bootstrap-server-stdout.txt",
            "main-server.txt",
            "main-server-stdout.txt",
            "post-check-server.txt",
            "post-check-server-stdout.txt",
        }
        observed_server_logs = {
            record.get("path")
            for record in server_logs.get("files", [])
            if isinstance(record, dict)
        }
        if observed_server_logs != expected_server_logs:
            add_reason(reasons, "server-log validation coverage is incomplete")
        if server_logs.get("failure_matches") != []:
            add_reason(reasons, "server logs contain fatal/checker failure markers")
    if not server_logs_passed:
        add_reason(reasons, "TiDB server-log validation did not pass")

    if not schema_bootstrap_passed:
        add_reason(reasons, "TPC-C schema bootstrap did not pass")
    if not preparation_passed:
        add_reason(reasons, "TPC-C deterministic preparation reference did not pass")
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
    expected_transaction_weights: tuple[int, ...] = DEFAULT_TRANSACTION_WEIGHTS,
) -> dict[str, Any]:
    mode_dir = root / "trials" / f"trial-{trial_index:03d}" / mode
    attempts = attempt_directories(mode_dir)
    if not attempts:
        row = new_row(trial_index, mode, workload_label, None, root)
        row["invalid_reason"] = "no attempt directory found"
        return row
    parsed = [
        parse_attempt(
            root,
            path,
            trial_index,
            mode,
            workload_label,
            expected_transaction_weights,
        )
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


def apply_paired_overhead(
    rows: list[dict[str, Any]], active_modes: tuple[str, ...]
) -> None:
    by_trial: dict[int, dict[str, dict[str, Any]]] = {}
    for row in rows:
        by_trial.setdefault(int(row["trial_index"]), {})[str(row["mode"])] = row
    for trial_modes in by_trial.values():
        baseline = trial_modes["baseline-original"]
        baseline_tpm = as_float(baseline.get("tpm_c"))
        baseline_fixed = as_float(
            baseline.get("fixed_window_new_order_transactions_per_minute")
        )
        if baseline["valid"] and baseline_tpm is not None and baseline_tpm > 0:
            baseline["paired_baseline_tpm_c"] = baseline_tpm
            baseline["throughput_overhead_percent"] = 0.0
        if baseline["valid"] and baseline_fixed is not None and baseline_fixed > 0:
            baseline[
                "paired_baseline_fixed_window_new_order_transactions_per_minute"
            ] = baseline_fixed
            baseline["fixed_window_throughput_overhead_percent"] = 0.0
        for mode in active_modes[1:]:
            checked = trial_modes[mode]
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
            checked_fixed = as_float(
                checked.get("fixed_window_new_order_transactions_per_minute")
            )
            checked_end_to_verdict = as_float(
                checked.get("end_to_verdict_new_order_transactions_per_minute")
            )
            if (
                baseline["valid"]
                and checked["valid"]
                and baseline_fixed is not None
                and baseline_fixed > 0
            ):
                checked[
                    "paired_baseline_fixed_window_new_order_transactions_per_minute"
                ] = baseline_fixed
                if checked_fixed is not None:
                    checked["fixed_window_throughput_overhead_percent"] = (
                        (baseline_fixed - checked_fixed) * 100.0 / baseline_fixed
                    )
                if checked_end_to_verdict is not None:
                    checked["end_to_verdict_throughput_overhead_percent"] = (
                        (baseline_fixed - checked_end_to_verdict)
                        * 100.0
                        / baseline_fixed
                    )


def numeric_distribution(values: Iterable[Any]) -> tuple[float | None, float | None, float | None]:
    numbers = [number for value in values if (number := as_float(value)) is not None]
    if not numbers:
        return None, None, None
    return statistics.median(numbers), min(numbers), max(numbers)


def summarize(
    rows: list[dict[str, Any]],
    modes: tuple[str, ...],
) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for mode in modes:
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

def performance_rows(
    summary_rows: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for source in summary_rows:
        row = {column: source.get(column) for column in PERFORMANCE_COLUMNS}
        if source.get("mode") == "baseline-original":
            baseline_throughput = source.get(
                "fixed_window_new_order_transactions_per_minute_median"
            )
            row[
                "end_to_verdict_new_order_transactions_per_minute_median"
            ] = baseline_throughput
            row["end_to_verdict_throughput_overhead_percent_median"] = (
                0.0 if as_float(baseline_throughput) is not None else None
            )
        rows.append(row)
    return rows



def format_number(value: Any, digits: int = 2, suffix: str = "") -> str:
    number = as_float(value)
    return "—" if number is None else f"{number:.{digits}f}{suffix}"


def markdown(summary: dict[str, Any]) -> str:
    weights = tuple(summary.get("transaction_weights", DEFAULT_TRANSACTION_WEIGHTS))
    diagnostic_weights = weights != DEFAULT_TRANSACTION_WEIGHTS
    title = (
        "# TPC-C diagnostic-weight serialized global-lock comparison"
        if diagnostic_weights
        else "# TPC-C full-mix serialized global-lock comparison"
    )
    lines = [
        title,
        "",
        f"Result root: `{summary['result_root']}`",
        "",
        "Transaction weights (NewOrder, Payment, OrderStatus, Delivery, "
        f"StockLevel): `{','.join(str(value) for value in weights)}`",
        "",
        f"Overall validity: **{'PASS' if summary['all_valid'] else 'INVALID'}**",
        "",
        "## Per-trial results",
        "",
        "| Trial | Mode | Valid | tpmC | Fixed NO/min | End-to-verdict NO/min | Fixed OH | E2V OH | Live | Tail tx/ev | Samples cov/gap | p99 | Error | CPU | RSS | Captured ev/U/R B | Queue peak d/B | Lock wait avg/max | Lock hold avg/max | Start-to-verdict | Drain | Native close | Shutdown |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in summary["trial_rows"]:
        lines.append(
            "| {trial} | {mode} | {valid} | {tpm} | {fixed} | {end_to_verdict} | {fixed_overhead} | {end_to_verdict_overhead} | {live} | {tail} | {samples} | {p99} | {error} | {cpu} | {rss} | {capture} | {queue} | {lock_wait} | {lock_hold} | {verdict} | {drain} | {close} | {shutdown} |".format(
                trial=row["trial"],
                mode=row["mode"],
                valid="yes" if row["valid"] else "no",
                tpm=format_number(row.get("tpm_c")),
                fixed=format_number(
                    row.get("fixed_window_new_order_transactions_per_minute")
                ),
                end_to_verdict=format_number(
                    row.get("end_to_verdict_new_order_transactions_per_minute")
                ),
                fixed_overhead=format_number(
                    row.get("fixed_window_throughput_overhead_percent"), 2, "%"
                ),
                end_to_verdict_overhead=format_number(
                    row.get("end_to_verdict_throughput_overhead_percent"), 2, "%"
                ),
                p99=format_number(row.get("new_order_p99_latency_ms"), 2, " ms"),
                live=(
                    "pass" if row.get("live_sustainability_pass") is True
                    else "fail" if row.get("live_sustainability_pass") is False
                    else "—"
                ),
                tail=(f"{format_number(row.get('cutoff_residual_transactions'), 0)}/"
                      f"{format_number(row.get('cutoff_residual_events'), 0)}"),
                samples=(f"{format_number(row.get('checker_sample_coverage_percent'), 2, '%')}/"
                         f"{format_number(row.get('checker_max_sample_gap_seconds'), 2, ' s')}"),
                error=format_number(row.get("new_order_error_rate_percent"), 2, "%"),
                cpu=format_number(row.get("tidb_cpu_mean_percent"), 2, "%"),
                rss=format_number(row.get("tidb_max_rss_kib"), 0, " KiB"),
                capture=(f"{format_number(row.get('captured_events'), 0)}/"
                         f"{format_number(row.get('captured_used_bytes'), 0)}/"
                         f"{format_number(row.get('captured_reserved_bytes'), 0)}"),
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
            "| Mode | Valid/invalid | tpmC | Fixed NO/min | End-to-verdict NO/min | Fixed OH | E2V OH | Tail tx/ev | Peak backlog tx/ev | Samples cov/gap | p99 | Error | Captured ev/U/R B | Queue peak | Lock wait avg | Lock hold avg | Start-to-verdict | Drain | Native close | Shutdown |",
            "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
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
            "| {mode} | {valid}/{invalid} | {tpm} | {fixed} | {end_to_verdict} | {fixed_overhead} | {end_to_verdict_overhead} | {tail} | {backlog} | {samples} | {p99} | {error} | {capture} | {queue} | {lock_wait} | {lock_hold} | {verdict} | {drain} | {close} | {shutdown} |".format(
                mode=row["mode"],
                valid=row["valid_trial_count"],
                invalid=row["invalid_trial_count"],
                tpm=ranged("tpm_c"),
                fixed=ranged("fixed_window_new_order_transactions_per_minute"),
                end_to_verdict=ranged(
                    "end_to_verdict_new_order_transactions_per_minute"
                ),
                fixed_overhead=ranged(
                    "fixed_window_throughput_overhead_percent", 2, "%"
                ),
                end_to_verdict_overhead=ranged(
                    "end_to_verdict_throughput_overhead_percent", 2, "%"
                ),
                p99=ranged("new_order_p99_latency_ms", 2, " ms"),
                tail=(f"{ranged('cutoff_residual_transactions', 0)}/"
                      f"{ranged('cutoff_residual_events', 0)}"),
                backlog=(f"{ranged('workload_peak_transaction_backlog', 0)}/"
                         f"{ranged('workload_peak_event_backlog', 0)}"),
                samples=(f"{ranged('checker_sample_coverage_percent', 2, '%')}/"
                         f"{ranged('checker_max_sample_gap_seconds', 2, ' s')}"),
                error=ranged("new_order_error_rate_percent", 2, "%"),
                capture=(f"{ranged('captured_events', 0)}/"
                         f"{ranged('captured_used_bytes', 0)}/"
                         f"{ranged('captured_reserved_bytes', 0)}"),
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
            "- `tpmC` is go-tpc's fixed-window DB-facing metric; it never includes checker drain time.",
            "- Fixed NO/min is measured NewOrder count divided by the actual validated client start-to-exit interval `T`.",
            "- End-to-verdict NO/min uses the same count over the directly measured client start-to-checker-completion interval. A prefix verdict never ends this denominator; it ends only after every accepted publication and event reaches the mode target and all ownership is zero.",
            "- Exact residual conservation, final zero ownership, scrape coverage/gaps, and consumer progress remain validity gates.",
            "- A cutoff tail over 1%, drain over 30 seconds, or sustained backlog growth makes the live-sustainability diagnostic fail, but does not invalidate an exactly drained run; the complete tail is charged to end-to-verdict throughput.",
            "- Fixed and end-to-verdict overhead are paired with the valid pristine baseline's fixed NO/min from the same trial.",
            "- `captured_minus_*` columns are literal native capture counters and may include prepared attempts that later aborted. `accepted_publications_minus_*` columns are the required serialized publication tails used for completeness.",
            "- Queue backpressure is reported but does not by itself invalidate a run; fatal timeout, lost evidence, or undrained ownership does.",
            "- Captured used/reserved bytes are cumulative native packed-batch totals over the complete checker metric scope, not live ownership at close.",
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
    if experiment.get("format_version") != 2:
        raise AnalysisError("experiment.json format_version must be 2")
    if experiment.get("workload_label") != "TPC-C full mix":
        raise AnalysisError("experiment.json workload_label must be TPC-C full mix")
    configured_weights = experiment.get("transaction_weights")
    if (
        not isinstance(configured_weights, list)
        or len(configured_weights) != len(DEFAULT_TRANSACTION_WEIGHTS)
        or any(
            isinstance(value, bool) or not isinstance(value, int) or value < 0
            for value in configured_weights
        )
        or sum(configured_weights) != 100
        or configured_weights[0] <= 0
    ):
        raise AnalysisError(
            "experiment.json transaction weights must be five nonnegative integers "
            "summing to 100 with a positive NewOrder weight"
        )
    transaction_weights = tuple(configured_weights)
    if (
        transaction_weights != DEFAULT_TRANSACTION_WEIGHTS
        and experiment.get("smoke") is not True
    ):
        raise AnalysisError(
            "non-default transaction weights are supported only for smoke diagnostics"
        )
    workload_configuration = experiment.get("workload", {})
    preparation_configuration = experiment.get("preparation", {})
    storage_configuration = experiment.get("storage_configuration", {})
    if workload_configuration.get("prepare_threads") != 1:
        raise AnalysisError("experiment preparation worker count must be 1")
    if workload_configuration.get("go_tpc_ignore_error") is not False:
        raise AnalysisError("experiment must disable generic go-tpc ignore-error")
    if workload_configuration.get("go_tpc_silence_errors") is not True:
        raise AnalysisError("experiment must suppress per-error logging")
    if workload_configuration.get("go_tpc_error_policy") != {
        "policy": "continue-only-tidb-write-conflict",
        "mysql_error_numbers": [9007],
        "generic_ignore_error": False,
    }:
        raise AnalysisError(
            "experiment must continue only classified TiDB Error 9007 conflicts"
        )
    if preparation_configuration.get("initial_state") != "fresh-empty-store-per-mode":
        raise AnalysisError("experiment must start every mode from a fresh empty store")
    if preparation_configuration.get("partition_type") != {"code": 1, "name": "HASH"}:
        raise AnalysisError("experiment preparation partition type must be HASH (code 1)")
    if preparation_configuration.get("partitions") != 1:
        raise AnalysisError("experiment preparation partition count must be 1")
    if preparation_configuration.get("use_foreign_keys") is not False:
        raise AnalysisError("experiment preparation must disable foreign keys")
    if storage_configuration.get("coprocessor_cache_capacity_mb") != 0:
        raise AnalysisError("experiment must disable coprocessor result caching")
    if storage_configuration.get("tidb_store_batch_size") != 0:
        raise AnalysisError("experiment must disable store batching")
    if storage_configuration.get("tidb_allow_batch_cop") is not False:
        raise AnalysisError("experiment must disable BatchCop")
    if storage_configuration.get("tidb_index_lookup_pushdown_policy") != "force":
        raise AnalysisError("experiment must force index-lookup pushdown")
    trials = as_int(experiment.get("trials"))
    if trials is None or trials <= 0:
        raise AnalysisError("experiment.json trials must be a positive integer")
    if not experiment.get("smoke", False) and trials < 5:
        raise AnalysisError("measured experiments require at least five paired trials")
    configured_modes = experiment.get("modes")
    allowed_mode_matrices = (MODES, BASELINE_FULL_MODES)
    if (
        not isinstance(configured_modes, list)
        or tuple(configured_modes) not in allowed_mode_matrices
    ):
        raise AnalysisError("experiment.json modes do not match a supported mode matrix")
    if experiment.get("mode_order") != configured_modes:
        raise AnalysisError("experiment.json mode_order does not match modes")
    active_modes = tuple(configured_modes)
    workload_label = "TPC-C full mix"

    rows = [
        select_attempt(
            root,
            trial,
            mode,
            workload_label,
            transaction_weights,
        )
        for trial in range(1, trials + 1)
        for mode in active_modes
    ]
    apply_paired_overhead(rows, active_modes)
    cleaned_rows = [clean_row(row) for row in rows]
    summary_rows = summarize(cleaned_rows, active_modes)
    all_valid = (
        len(cleaned_rows) == trials * len(active_modes)
        and all(row["valid"] for row in cleaned_rows)
    )
    return {
        "schema_version": 1,
        "result_root": root.as_posix(),
        "workload_label": workload_label,
        "transaction_weights": list(transaction_weights),
        "expected_trials": trials,
        "modes": list(active_modes),
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
        root / "tpcc-performance.csv",
        render_csv(performance_rows(summary["summary_rows"]), PERFORMANCE_COLUMNS),
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
        "tpcc-performance.csv",
        "tpcc-summary.csv",
        "tpcc-summary.json",
        "tpcc-report.md",
    ):
        print(f"wrote {root / name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
