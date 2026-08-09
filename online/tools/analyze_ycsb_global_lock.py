#!/usr/bin/env python3
"""Validate and summarize deterministic paired YCSB online-checker runs.

The runner writes append-only attempts below::

    trials/workload-A/trial-001/MODE/attempt-001/

Only the matrix declared by ``experiment.json`` is authoritative.  A newer
valid completed attempt supersedes an older one; failed and incomplete attempts
remain visible and a missing matrix entry always emits an invalid trial row.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
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
EXPECTED_OUTCOME = {
    "serialized-capture-only": "capture-only-complete",
    "serialized-order-only": "order-only-complete",
    "serialized-full": "pass",
}
WORKLOADS = tuple("ABCDEF")
ATTEMPT_RE = re.compile(r"^attempt-(\d+)$")
RESULT_FORMAT = "go-ycsb-online-result-v1"


TRIAL_COLUMNS = (
    "workload",
    "trial",
    "mode",
    "attempt",
    "attempt_path",
    "valid",
    "invalid_reason",
    "outcome",
    "verdict",
    "requested_operations",
    "successful_operations",
    "failed_operations",
    "actual_operation_mix_json",
    "db_call_mix_json",
    "fixed_duration_seconds",
    "drain_duration_seconds",
    "completion_duration_seconds",
    "fixed_ops_per_second",
    "completion_ops_per_second",
    "paired_baseline_fixed_ops_per_second",
    "paired_baseline_completion_ops_per_second",
    "paired_fixed_overhead_percent",
    "paired_completion_overhead_percent",
    "read_p50_us",
    "read_p95_us",
    "read_p99_us",
    "read_p99_9_us",
    "update_p50_us",
    "update_p95_us",
    "update_p99_us",
    "update_p99_9_us",
    "insert_p50_us",
    "insert_p95_us",
    "insert_p99_us",
    "insert_p99_9_us",
    "scan_p50_us",
    "scan_p95_us",
    "scan_p99_us",
    "scan_p99_9_us",
    "rmw_p50_us",
    "rmw_p95_us",
    "rmw_p99_us",
    "rmw_p99_9_us",
    "initial_row_count",
    "warmup_insert_count",
    "measured_insert_count",
    "measured_delete_count",
    "final_row_count",
    "load_schema_sha256",
    "load_data_sha256",
    "load_combined_sha256",
    "tidb_cpu_seconds",
    "tidb_cpu_mean_percent",
    "tidb_peak_rss_kib",
    "tidb_peak_hwm_kib",
    "client_cpu_seconds",
    "client_peak_rss_kib",
    "checker_sample_count",
    "checker_sample_coverage_percent",
    "checker_max_sample_gap_seconds",
    "cutoff_observation_delay_seconds",
    "accepted_publications",
    "accepted_events",
    "ordered_transactions",
    "checked_transactions",
    "checked_events",
    "measured_accepted_publications",
    "measured_accepted_events",
    "measured_ordered_transactions",
    "measured_checked_transactions",
    "measured_checked_events",
    "measured_attempts_registered",
    "measured_attempts_published",
    "measured_attempts_aborted",
    "measured_attempt_abort_percent",
    "total_attempts_aborted",
    "checker_transaction_service_rate_per_second",
    "checker_event_service_rate_per_second",
    "workload_peak_transaction_backlog",
    "workload_peak_event_backlog",
    "workload_peak_queue_depth",
    "workload_peak_queue_bytes",
    "queue_backpressure_count",
    "queue_backpressure_time_ns",
    "capture_peak_bytes",
    "retained_state_peak_bytes",
    "global_lock_wait_ns",
    "global_lock_hold_ns",
    "finalization_wait_ns",
    "cutoff_residual_transactions",
    "cutoff_residual_events",
    "drained_transactions",
    "drained_events",
    "active_attempts_at_close",
    "active_finalizers_at_close",
    "active_fragments_at_close",
    "tail_percent",
    "sustained_backlog_growth",
    "sustainability_warning",
)

SUMMARY_METRICS = (
    "fixed_ops_per_second",
    "completion_ops_per_second",
    "paired_fixed_overhead_percent",
    "paired_completion_overhead_percent",
    "read_p99_us",
    "update_p99_us",
    "insert_p99_us",
    "scan_p99_us",
    "rmw_p99_us",
    "tidb_cpu_seconds",
    "tidb_cpu_mean_percent",
    "tidb_peak_rss_kib",
    "client_cpu_seconds",
    "checker_transaction_service_rate_per_second",
    "checker_event_service_rate_per_second",
    "measured_attempts_aborted",
    "measured_attempt_abort_percent",
    "workload_peak_transaction_backlog",
    "workload_peak_event_backlog",
    "workload_peak_queue_bytes",
    "capture_peak_bytes",
    "retained_state_peak_bytes",
    "drain_duration_seconds",
    "tail_percent",
)


class AnalysisError(RuntimeError):
    """The experiment declaration itself is malformed or unsafe to analyze."""


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_sha256(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def load_json(path: pathlib.Path, reasons: list[str], label: str) -> Any:
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        reasons.append(f"{label} is unavailable or malformed: {error}")
        return {}


def add_reason(reasons: list[str], condition: bool, message: str) -> None:
    if not condition and message not in reasons:
        reasons.append(message)


def integer(value: Any) -> int | None:
    return value if isinstance(value, int) and not isinstance(value, bool) else None


def number(value: Any) -> float | None:
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        result = float(value)
        return result if math.isfinite(result) else None
    return None


def nested(record: Any, *path: str) -> Any:
    value = record
    for name in path:
        if not isinstance(value, dict):
            return None
        value = value.get(name)
    return value


def require_pass(record: Any, reasons: list[str], label: str) -> None:
    add_reason(
        reasons,
        isinstance(record, dict) and record.get("passed") is True,
        f"{label} did not pass",
    )


def validate_experiment(experiment: Any) -> dict[str, Any]:
    if not isinstance(experiment, dict):
        raise AnalysisError("experiment.json must contain an object")
    if experiment.get("format_version") != 1:
        raise AnalysisError("experiment format_version must be 1")
    modes = experiment.get("modes")
    diagnostic = experiment.get("diagnostic_only") is True
    if diagnostic:
        if (
            not isinstance(modes, list)
            or len(modes) < 2
            or modes[0] != MODES[0]
            or any(mode not in MODES for mode in modes)
            or len(set(modes)) != len(modes)
        ):
            raise AnalysisError("diagnostic modes require baseline-original followed by unique checked modes")
    elif modes != list(MODES):
        raise AnalysisError(f"headline mode matrix must be exactly {list(MODES)!r}")
    workloads = experiment.get("workloads")
    if (
        not isinstance(workloads, list)
        or not workloads
        or any(item not in WORKLOADS for item in workloads)
        or len(set(workloads)) != len(workloads)
    ):
        raise AnalysisError("workloads must be a unique nonempty subset of A-F")
    trials = integer(experiment.get("trials"))
    if trials is None or trials <= 0:
        raise AnalysisError("trials must be a positive integer")
    smoke = experiment.get("smoke") is True
    if not smoke and not diagnostic:
        if workloads != list(WORKLOADS):
            raise AnalysisError("publishable experiments require workloads A-F in order")
        if trials < 5:
            raise AnalysisError("publishable experiments require at least five trials")
        calibration = experiment.get("calibration")
        if not isinstance(calibration, dict) or not calibration.get("sha256"):
            raise AnalysisError("publishable experiments require a calibration hash")
    rotation = experiment.get("mode_rotation")
    if not isinstance(rotation, dict):
        raise AnalysisError("mode_rotation must be declared")
    for trial in range(1, trials + 1):
        order = rotation.get(str(trial))
        if not isinstance(order, list) or sorted(order) != sorted(modes):
            raise AnalysisError(f"mode_rotation[{trial}] is not a permutation of declared modes")
    return experiment


def validate_client_result(
    record: Any,
    reasons: list[str],
    label: str,
    requested: int | None,
) -> None:
    add_reason(reasons, isinstance(record, dict), f"{label} result is not an object")
    if not isinstance(record, dict):
        return
    add_reason(reasons, record.get("format_version") == RESULT_FORMAT, f"{label} result format mismatch")
    got_requested = integer(record.get("requested"))
    attempted = integer(record.get("attempted"))
    successful = integer(record.get("successful"))
    failed = integer(record.get("failed"))
    if requested is not None:
        add_reason(reasons, got_requested == requested, f"{label} requested count mismatch")
    add_reason(
        reasons,
        None not in (got_requested, attempted, successful, failed)
        and got_requested == attempted
        and attempted == successful
        and failed == 0,
        f"{label} operation lifecycle is not conserved",
    )
    add_reason(reasons, record.get("error_classes") == {}, f"{label} has error classes")


def metric_count(record: Any, name: str) -> int:
    value = nested(record, "measurements", name, "count")
    return value if isinstance(value, int) and value >= 0 else 0


def operation_mix(record: Any, workload: str) -> tuple[dict[str, int], dict[str, int]]:
    total = integer(record.get("successful")) if isinstance(record, dict) else None
    total = total or 0
    raw = {name: metric_count(record, name) for name in ("READ", "UPDATE", "INSERT", "SCAN", "READ_MODIFY_WRITE", "DELETE")}
    if workload == "F":
        rmw = raw["READ_MODIFY_WRITE"]
        top = {"READ": total - rmw, "READ_MODIFY_WRITE": rmw}
        db = {"READ": raw["READ"], "UPDATE": raw["UPDATE"]}
    else:
        top = {name: raw[name] for name in ("READ", "UPDATE", "INSERT", "SCAN", "DELETE") if raw[name]}
        db = dict(top)
    return top, db


def validate_checker_result(record: Any, mode: str, reasons: list[str]) -> None:
    if mode == MODES[0]:
        add_reason(reasons, not record, "baseline unexpectedly produced a checker result")
        return
    pipeline = PIPELINE_MODE[mode]
    add_reason(reasons, isinstance(record, dict), "checker result is missing")
    if not isinstance(record, dict):
        return
    add_reason(reasons, record.get("format_version") == 1, "checker result format mismatch")
    add_reason(reasons, record.get("mode") == pipeline, "checker mode mismatch")
    add_reason(reasons, record.get("ordering_protocol") == "serialized-global-lock", "ordering protocol mismatch")
    add_reason(reasons, record.get("outcome") == EXPECTED_OUTCOME[mode], "checker outcome mismatch")
    add_reason(reasons, record.get("close_status") == 0, "checker close status is nonzero")
    add_reason(reasons, record.get("runtime_error") is None and not record.get("close_error"), "checker reported an error")
    adapter = record.get("adapter_metrics", {})
    native = record.get("native_metrics", {})
    add_reason(reasons, isinstance(adapter, dict) and isinstance(native, dict), "checker metrics are missing")
    if not isinstance(adapter, dict) or not isinstance(native, dict):
        return
    registered, published, aborted = (integer(adapter.get(name)) for name in ("attempts_registered", "attempts_published", "attempts_aborted"))
    accepted = integer(adapter.get("accepted_publications"))
    add_reason(reasons, None not in (registered, published, aborted) and registered == published + aborted, "attempt counters do not balance")
    add_reason(reasons, accepted is not None and accepted > 0 and accepted == published, "publication counters do not balance")
    fragments_started = integer(adapter.get("fragments_started"))
    fragments_accepted = integer(adapter.get("fragments_accepted"))
    fragments_discarded = integer(adapter.get("fragments_discarded"))
    add_reason(
        reasons,
        None not in (fragments_started, fragments_accepted, fragments_discarded)
        and fragments_started > 0
        and fragments_started == fragments_accepted + fragments_discarded,
        "fragment counters do not balance",
    )
    for field in ("fatal_calls", "publish_errors", "abort_errors", "fragment_capture_errors"):
        add_reason(reasons, adapter.get(field) == 0, f"adapter {field} is nonzero")
    for field in ("active_attempts", "active_finalizers", "active_fragments", "fragment_buffer_active_bytes", "fragment_retained_active_bytes"):
        add_reason(reasons, adapter.get(field) == 0, f"adapter {field} did not drain")
    for field in ("active_attempts", "queue_reserved_bytes", "queue_reserved_depth", "queue_bytes", "queue_depth", "reorder_bytes"):
        add_reason(reasons, native.get(field) == 0, f"native {field} did not drain")
    add_reason(reasons, native.get("fatal") is False, "native fatal is set")
    add_reason(reasons, integer(native.get("captured_transactions")) is not None and native.get("captured_transactions") > 0, "captured transaction count did not advance")
    add_reason(reasons, integer(native.get("captured_events")) is not None and native.get("captured_events") > 0, "captured event count did not advance")
    native_accepted = integer(native.get("accepted_publications"))
    add_reason(reasons, native_accepted == accepted, "adapter/native accepted counts differ")
    if pipeline == "capture-only":
        add_reason(reasons, native.get("recycled_transactions") == native_accepted, "capture-only recycling is incomplete")
    else:
        add_reason(reasons, native.get("ordered_transactions") == native_accepted, "ordering is incomplete")
    if pipeline == "full":
        add_reason(reasons, native.get("checked_transactions") == native_accepted, "checking is incomplete")
        add_reason(reasons, native.get("checked_events") == native.get("accepted_events"), "checked event count is incomplete")
        add_reason(reasons, native.get("violating_transactions") == 0, "checker found a violation")
        add_reason(reasons, nested(record, "verdict", "status") == "pass", "full verdict is not pass")


def new_row(workload: str, trial: int, mode: str, path: pathlib.Path | None) -> dict[str, Any]:
    row = {name: None for name in TRIAL_COLUMNS}
    row.update({
        "workload": workload,
        "trial": trial,
        "mode": mode,
        "attempt": path.name if path else None,
        "attempt_path": str(path) if path else None,
        "valid": False,
        "invalid_reason": "attempt is missing" if path is None else "",
    })
    return row


def parse_attempt(
    root: pathlib.Path,
    experiment: dict[str, Any],
    workload: str,
    trial: int,
    mode: str,
    attempt: pathlib.Path,
) -> dict[str, Any]:
    row = new_row(workload, trial, mode, attempt)
    reasons: list[str] = []
    expected_identity = experiment.get("experiment_sha256")
    if not isinstance(expected_identity, str):
        unsigned = dict(experiment)
        unsigned.pop("experiment_sha256", None)
        expected_identity = canonical_sha256(unsigned)

    configuration = load_json(attempt / "configuration.json", reasons, "configuration")
    completed = load_json(attempt / "completed.json", reasons, "completed marker")
    status = load_json(attempt / "runner-status.json", reasons, "runner status")
    add_reason(reasons, configuration.get("experiment_sha256") == expected_identity, "attempt experiment manifest mismatch")
    add_reason(reasons, configuration.get("workload") == workload, "attempt workload mismatch")
    add_reason(reasons, configuration.get("trial") == trial, "attempt trial mismatch")
    add_reason(reasons, configuration.get("mode") == mode, "attempt mode mismatch")
    add_reason(reasons, completed.get("status") == "complete", "attempt is not complete")
    add_reason(reasons, status.get("status") == "complete", "runner did not finish successfully")

    config = experiment.get("configuration", {})
    requested = integer(nested(config, "workloads", workload, "measured_operations"))
    warmup_requested = integer(nested(config, "workloads", workload, "warmup_operations"))
    record_count = integer(config.get("record_count"))
    load_result = load_json(attempt / "load-result.json", reasons, "load result")
    warmup_result = load_json(attempt / "warmup-result.json", reasons, "warmup result")
    measured_result = load_json(attempt / "workload-result.json", reasons, "measured result")
    validate_client_result(load_result, reasons, "load", record_count)
    validate_client_result(warmup_result, reasons, "warmup", warmup_requested)
    validate_client_result(measured_result, reasons, "measured", requested)

    validations: dict[str, Any] = {}
    for filename, label in (
        ("schema-validation.json", "schema validation"),
        ("load-validation.json", "load validation"),
        ("gate-validation.json", "capability gate"),
        ("warmup-validation.json", "warmup validation"),
        ("workload-validation.json", "workload validation"),
        ("load-drain.json", "load drain"),
        ("gate-drain.json", "gate drain"),
        ("warmup-drain.json", "warmup drain"),
        ("workload-drain.json", "workload drain"),
        ("telemetry-validation.json", "telemetry validation"),
        ("post-check-validation.json", "post-check validation"),
        ("server-log-validation.json", "server log validation"),
    ):
        value = load_json(attempt / filename, reasons, label)
        validations[filename] = value
        require_pass(value, reasons, label)

    measured_validation = validations["workload-validation.json"]
    mix, db_mix = operation_mix(measured_result, workload)
    add_reason(reasons, measured_validation.get("actual_operation_mix") == mix, "actual operation mix mismatch")
    add_reason(reasons, measured_validation.get("db_call_mix") == db_mix, "DB call mix mismatch")
    if workload == "F":
        rmw = mix.get("READ_MODIFY_WRITE", 0)
        add_reason(reasons, db_mix.get("READ", 0) == (requested or 0), "workload F read expansion mismatch")
        add_reason(reasons, db_mix.get("UPDATE", 0) == rmw, "workload F update expansion mismatch")

    load_digest = load_json(attempt / "load-digest.json", reasons, "load digest")
    post_digest = load_json(attempt / "post-check-digest.json", reasons, "post-check digest")
    initial_rows = integer(load_digest.get("total_rows"))
    final_rows = integer(post_digest.get("total_rows"))
    warmup_inserts = metric_count(warmup_result, "INSERT")
    measured_inserts = metric_count(measured_result, "INSERT")
    measured_deletes = metric_count(measured_result, "DELETE")
    add_reason(reasons, initial_rows == record_count, "initial row count mismatch")
    add_reason(
        reasons,
        None not in (initial_rows, final_rows)
        and final_rows == initial_rows + warmup_inserts + measured_inserts - measured_deletes,
        "final row-count conservation mismatch",
    )

    timing = load_json(attempt / "timing.json", reasons, "timing")
    start = integer(timing.get("workload_start_monotonic_ns"))
    client_exit = integer(timing.get("workload_client_exit_monotonic_ns"))
    drain_complete = integer(timing.get("checker_drain_complete_monotonic_ns"))
    add_reason(reasons, None not in (start, client_exit) and client_exit > start, "measured timing is not monotonic")
    add_reason(reasons, drain_complete is not None and client_exit is not None and drain_complete >= client_exit, "drain timing is not monotonic")
    fixed_duration = (client_exit - start) / 1e9 if None not in (start, client_exit) and client_exit > start else None
    drain_duration = (drain_complete - client_exit) / 1e9 if None not in (drain_complete, client_exit) and drain_complete >= client_exit else None
    completion_duration = (drain_complete - start) / 1e9 if None not in (drain_complete, start) and drain_complete > start else None
    successful = integer(measured_result.get("successful"))

    checker_result: Any = {}
    checker_path = attempt / "checker-result.json"
    if checker_path.exists():
        checker_result = load_json(checker_path, reasons, "checker result")
    validate_checker_result(checker_result, mode, reasons)

    telemetry = validations["telemetry-validation.json"]
    drain = validations["workload-drain.json"]
    resource = load_json(attempt / "resource-summary.json", reasons, "resource summary")
    client_resource = load_json(attempt / "workload-resource.json", reasons, "client resource summary")
    native = checker_result.get("native_metrics", {}) if isinstance(checker_result, dict) else {}
    adapter = checker_result.get("adapter_metrics", {}) if isinstance(checker_result, dict) else {}
    attempt_deltas = {name: None for name in ("registered", "published", "aborted")}
    if mode != MODES[0]:
        workload_start = load_json(attempt / "workload-start.json", reasons, "workload-start snapshot")
        workload_cutoff = load_json(attempt / "workload-cutoff.json", reasons, "workload-cutoff snapshot")
        add_reason(reasons, workload_start.get("scrape_success") is True, "workload-start scrape failed")
        add_reason(reasons, workload_cutoff.get("scrape_success") is True, "workload-cutoff scrape failed")
        start_adapter = workload_start.get("adapter", {})
        cutoff_adapter = workload_cutoff.get("adapter", {})
        for name in attempt_deltas:
            field = f"attempts_{name}"
            start_value = number(start_adapter.get(field)) if isinstance(start_adapter, dict) else None
            cutoff_value = number(cutoff_adapter.get(field)) if isinstance(cutoff_adapter, dict) else None
            add_reason(
                reasons,
                start_value is not None and cutoff_value is not None and cutoff_value >= start_value,
                f"measured {field} delta is invalid",
            )
            if start_value is not None and cutoff_value is not None and cutoff_value >= start_value:
                attempt_deltas[name] = cutoff_value - start_value
        add_reason(
            reasons,
            None not in attempt_deltas.values()
            and math.isclose(
                attempt_deltas["registered"],
                attempt_deltas["published"] + attempt_deltas["aborted"],
                rel_tol=0.0,
                abs_tol=0.5,
            ),
            "measured attempt deltas do not balance",
        )
    measured_attempt_abort_percent = None
    if attempt_deltas["registered"]:
        measured_attempt_abort_percent = 100.0 * attempt_deltas["aborted"] / attempt_deltas["registered"]
    row.update({
        "outcome": checker_result.get("outcome") if mode != MODES[0] else "checker-disabled",
        "verdict": nested(checker_result, "verdict", "status") if mode != MODES[0] else None,
        "requested_operations": measured_result.get("requested"),
        "successful_operations": successful,
        "failed_operations": measured_result.get("failed"),
        "actual_operation_mix_json": json.dumps(mix, sort_keys=True, separators=(",", ":")),
        "db_call_mix_json": json.dumps(db_mix, sort_keys=True, separators=(",", ":")),
        "fixed_duration_seconds": fixed_duration,
        "drain_duration_seconds": drain_duration,
        "completion_duration_seconds": completion_duration,
        "fixed_ops_per_second": successful / fixed_duration if successful is not None and fixed_duration else None,
        "completion_ops_per_second": successful / completion_duration if successful is not None and completion_duration else None,
        "initial_row_count": initial_rows,
        "warmup_insert_count": warmup_inserts,
        "measured_insert_count": measured_inserts,
        "measured_delete_count": measured_deletes,
        "final_row_count": final_rows,
        "load_schema_sha256": load_digest.get("schema_digest_sha256"),
        "load_data_sha256": load_digest.get("data_digest_sha256"),
        "load_combined_sha256": load_digest.get("database_digest_sha256"),
        "tidb_cpu_seconds": resource.get("process_cpu_seconds"),
        "tidb_cpu_mean_percent": resource.get("cpu_mean_percent"),
        "tidb_peak_rss_kib": resource.get("maximum_rss_kib"),
        "tidb_peak_hwm_kib": resource.get("maximum_hwm_kib"),
        "client_cpu_seconds": client_resource.get("user_system_cpu_seconds"),
        "client_peak_rss_kib": client_resource.get("maximum_resident_set_size_kib"),
        "checker_sample_count": telemetry.get("sample_count"),
        "checker_sample_coverage_percent": telemetry.get("sample_coverage_percent"),
        "checker_max_sample_gap_seconds": telemetry.get("max_sample_gap_seconds"),
        "cutoff_observation_delay_seconds": telemetry.get("cutoff_observation_delay_seconds"),
        "accepted_publications": native.get("accepted_publications"),
        "accepted_events": native.get("accepted_events"),
        "ordered_transactions": native.get("ordered_transactions"),
        "checked_transactions": native.get("checked_transactions"),
        "checked_events": native.get("checked_events"),
        "measured_accepted_publications": telemetry.get("accepted_publications_delta"),
        "measured_accepted_events": telemetry.get("accepted_events_delta"),
        "measured_ordered_transactions": telemetry.get("ordered_transactions_delta"),
        "measured_checked_transactions": telemetry.get("checked_transactions_delta"),
        "measured_checked_events": telemetry.get("checked_events_delta"),
        "measured_attempts_registered": attempt_deltas["registered"],
        "measured_attempts_published": attempt_deltas["published"],
        "measured_attempts_aborted": attempt_deltas["aborted"],
        "measured_attempt_abort_percent": measured_attempt_abort_percent,
        "total_attempts_aborted": adapter.get("attempts_aborted"),
        "checker_transaction_service_rate_per_second": telemetry.get("transaction_service_rate_per_second"),
        "checker_event_service_rate_per_second": telemetry.get("event_service_rate_per_second"),
        "workload_peak_transaction_backlog": telemetry.get("peak_transaction_backlog"),
        "workload_peak_event_backlog": telemetry.get("peak_event_backlog"),
        "workload_peak_queue_depth": telemetry.get("peak_queue_depth"),
        "workload_peak_queue_bytes": telemetry.get("peak_queue_bytes"),
        "queue_backpressure_count": native.get("queue_backpressure_wait_count"),
        "queue_backpressure_time_ns": native.get("queue_backpressure_ns"),
        "capture_peak_bytes": telemetry.get("capture_peak_bytes"),
        "retained_state_peak_bytes": telemetry.get("retained_state_peak_bytes"),
        "global_lock_wait_ns": adapter.get("finalization_wait_nanos"),
        "global_lock_hold_ns": adapter.get("finalization_hold_nanos"),
        "finalization_wait_ns": adapter.get("finalization_wait_nanos"),
        "cutoff_residual_transactions": drain.get("cutoff_residual_transactions"),
        "cutoff_residual_events": drain.get("cutoff_residual_events"),
        "drained_transactions": drain.get("drained_transactions"),
        "drained_events": drain.get("drained_events"),
        "active_attempts_at_close": adapter.get("active_attempts"),
        "active_finalizers_at_close": adapter.get("active_finalizers"),
        "active_fragments_at_close": adapter.get("active_fragments"),
        "tail_percent": telemetry.get("tail_percent"),
        "sustained_backlog_growth": telemetry.get("sustained_backlog_growth"),
    })
    for prefix, metric_name in (
        ("read", "READ"),
        ("update", "UPDATE"),
        ("insert", "INSERT"),
        ("scan", "SCAN"),
        ("rmw", "READ_MODIFY_WRITE"),
    ):
        metric = nested(measured_result, "measurements", metric_name)
        metric = metric if isinstance(metric, dict) else {}
        for suffix in ("p50_us", "p95_us", "p99_us", "p99_9_us"):
            row[f"{prefix}_{suffix}"] = metric.get(suffix)

    warnings = []
    tail = number(row.get("tail_percent"))
    if tail is not None and tail > 1.0:
        warnings.append(f"tail {tail:.3f}% exceeds 1%")
    if drain_duration is not None and drain_duration > 30.0:
        warnings.append(f"drain {drain_duration:.3f}s exceeds 30s")
    if row.get("sustained_backlog_growth") is True:
        warnings.append("sustained backlog growth")
    row["sustainability_warning"] = "; ".join(warnings)
    row["valid"] = not reasons
    row["invalid_reason"] = "; ".join(reasons)
    return row


def attempt_directories(mode_dir: pathlib.Path) -> list[pathlib.Path]:
    if not mode_dir.is_dir():
        return []
    result = []
    for path in mode_dir.iterdir():
        match = ATTEMPT_RE.fullmatch(path.name)
        if path.is_dir() and match:
            result.append(path)
    return sorted(result, key=lambda path: int(ATTEMPT_RE.fullmatch(path.name).group(1)))


def select_attempt(
    root: pathlib.Path,
    experiment: dict[str, Any],
    workload: str,
    trial: int,
    mode: str,
) -> dict[str, Any]:
    mode_dir = root / "trials" / f"workload-{workload}" / f"trial-{trial:03d}" / mode
    paths = attempt_directories(mode_dir)
    if not paths:
        return new_row(workload, trial, mode, None)
    rows = [parse_attempt(root, experiment, workload, trial, mode, path) for path in reversed(paths)]
    return next((row for row in rows if row["valid"]), rows[0])


def apply_paired_overhead(rows: list[dict[str, Any]]) -> None:
    baselines = {
        (row["workload"], row["trial"]): row
        for row in rows
        if row["mode"] == MODES[0] and row["valid"]
    }
    for row in rows:
        if not row["valid"] or row["mode"] == MODES[0]:
            continue
        baseline = baselines.get((row["workload"], row["trial"]))
        if baseline is None:
            row["valid"] = False
            row["invalid_reason"] = "paired pristine baseline is invalid or missing"
            continue
        for rate, baseline_field, overhead in (
            ("fixed_ops_per_second", "paired_baseline_fixed_ops_per_second", "paired_fixed_overhead_percent"),
            ("completion_ops_per_second", "paired_baseline_completion_ops_per_second", "paired_completion_overhead_percent"),
        ):
            checked = number(row.get(rate))
            pristine = number(baseline.get(rate))
            row[baseline_field] = pristine
            row[overhead] = (1.0 - checked / pristine) * 100.0 if checked is not None and pristine and pristine > 0 else None


def distribution(values: Iterable[Any]) -> dict[str, Any]:
    data = [value for item in values if (value := number(item)) is not None]
    if not data:
        return {"count": 0, "median": None, "minimum": None, "maximum": None, "mad": None}
    median = statistics.median(data)
    return {
        "count": len(data),
        "median": median,
        "minimum": min(data),
        "maximum": max(data),
        "mad": statistics.median(abs(value - median) for value in data),
    }


def summarize(experiment: dict[str, Any], rows: list[dict[str, Any]]) -> dict[str, Any]:
    groups = []
    for workload in experiment["workloads"]:
        for mode in experiment["modes"]:
            members = [row for row in rows if row["workload"] == workload and row["mode"] == mode]
            valid = [row for row in members if row["valid"]]
            groups.append({
                "workload": workload,
                "mode": mode,
                "declared_trials": experiment["trials"],
                "valid_trials": len(valid),
                "invalid_trials": len(members) - len(valid),
                "metrics": {metric: distribution(row.get(metric) for row in valid) for metric in SUMMARY_METRICS},
            })
    return {
        "format_version": 1,
        "profile_id": experiment.get("profile_id"),
        "experiment_sha256": experiment.get("experiment_sha256"),
        "trials": experiment["trials"],
        "workloads": experiment["workloads"],
        "modes": experiment["modes"],
        "valid_rows": sum(bool(row["valid"]) for row in rows),
        "invalid_rows": sum(not bool(row["valid"]) for row in rows),
        "groups": groups,
        "rows": rows,
    }


def csv_value(value: Any) -> Any:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    return value


def render_csv(rows: list[dict[str, Any]], columns: Iterable[str]) -> str:
    output = io.StringIO()
    writer = csv.DictWriter(output, fieldnames=list(columns), lineterminator="\n")
    writer.writeheader()
    for row in rows:
        writer.writerow({name: csv_value(row.get(name)) for name in writer.fieldnames})
    return output.getvalue()


def summary_rows(summary: dict[str, Any]) -> list[dict[str, Any]]:
    result = []
    for group in summary["groups"]:
        row = {name: group[name] for name in ("workload", "mode", "declared_trials", "valid_trials", "invalid_trials")}
        for metric, values in group["metrics"].items():
            for statistic_name, value in values.items():
                row[f"{metric}_{statistic_name}"] = value
        result.append(row)
    return result


def performance_rows(summary: dict[str, Any]) -> list[dict[str, Any]]:
    result = []
    for group in summary["groups"]:
        metrics = group["metrics"]
        result.append({
            "workload": group["workload"],
            "mode": group["mode"],
            "valid_trials": group["valid_trials"],
            "median_fixed_ops_per_second": metrics["fixed_ops_per_second"]["median"],
            "median_completion_ops_per_second": metrics["completion_ops_per_second"]["median"],
            "median_paired_fixed_overhead_percent": metrics["paired_fixed_overhead_percent"]["median"],
            "median_paired_completion_overhead_percent": metrics["paired_completion_overhead_percent"]["median"],
            "median_measured_attempts_aborted": metrics["measured_attempts_aborted"]["median"],
            "median_measured_attempt_abort_percent": metrics["measured_attempt_abort_percent"]["median"],
        })
    return result


def fmt(value: Any, digits: int = 2) -> str:
    numeric = number(value)
    return "n/a" if numeric is None else f"{numeric:.{digits}f}"


def markdown(summary: dict[str, Any]) -> str:
    lines = [
        "# YCSB online-checker comparison",
        "",
        f"Profile: `{summary.get('profile_id')}`  ",
        f"Declared trials per workload/mode: {summary['trials']}  ",
        f"Valid rows: {summary['valid_rows']}; invalid rows: {summary['invalid_rows']}",
        "",
        "Completion rate means end-to-verdict for full mode and end-to-completion for capture/order modes.",
        "Workload F keeps one top-level RMW but expands it to one read and one update database transaction.",
        "",
        "| Workload | Mode | Valid | Fixed ops/s | Completion ops/s | Paired fixed overhead | Paired completion overhead | Measured aborts | Abort % |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for group in summary["groups"]:
        metrics = group["metrics"]
        lines.append(
            f"| {group['workload']} | {group['mode']} | {group['valid_trials']} | "
            f"{fmt(metrics['fixed_ops_per_second']['median'])} | "
            f"{fmt(metrics['completion_ops_per_second']['median'])} | "
            f"{fmt(metrics['paired_fixed_overhead_percent']['median'])}% | "
            f"{fmt(metrics['paired_completion_overhead_percent']['median'])}% | "
            f"{fmt(metrics['measured_attempts_aborted']['median'], 0)} | "
            f"{fmt(metrics['measured_attempt_abort_percent']['median'])}% |"
        )
    invalid = [row for row in summary["rows"] if not row["valid"]]
    warnings = [row for row in summary["rows"] if row.get("sustainability_warning")]
    lines.extend(["", "## Invalid attempts", ""])
    if invalid:
        for row in invalid:
            lines.append(f"- Workload {row['workload']} trial {row['trial']} `{row['mode']}`: {row['invalid_reason']}")
    else:
        lines.append("None.")
    lines.extend(["", "## Sustainability warnings", ""])
    if warnings:
        for row in warnings:
            lines.append(f"- Workload {row['workload']} trial {row['trial']} `{row['mode']}`: {row['sustainability_warning']}")
    else:
        lines.append("None.")
    return "\n".join(lines) + "\n"


def atomic_write(path: pathlib.Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w") as stream:
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_name, path)
    except BaseException:
        try:
            os.unlink(temporary_name)
        except OSError:
            pass
        raise


def analyze(root: pathlib.Path) -> dict[str, Any]:
    experiment_path = root / "experiment.json"
    try:
        experiment = validate_experiment(json.loads(experiment_path.read_text()))
    except (OSError, json.JSONDecodeError) as error:
        raise AnalysisError(f"cannot read experiment.json: {error}") from error
    rows = [
        select_attempt(root, experiment, workload, trial, mode)
        for workload in experiment["workloads"]
        for trial in range(1, experiment["trials"] + 1)
        for mode in experiment["modes"]
    ]
    apply_paired_overhead(rows)
    return summarize(experiment, rows)


def write_outputs(root: pathlib.Path, summary: dict[str, Any]) -> None:
    trial_csv = render_csv(summary["rows"], TRIAL_COLUMNS)
    aggregate = summary_rows(summary)
    aggregate_columns = list(aggregate[0]) if aggregate else []
    performance = performance_rows(summary)
    performance_columns = list(performance[0]) if performance else []
    public_summary = {key: value for key, value in summary.items() if key != "rows"}
    atomic_write(root / "ycsb-trials.csv", trial_csv)
    atomic_write(root / "ycsb-performance.csv", render_csv(performance, performance_columns))
    atomic_write(root / "ycsb-summary.csv", render_csv(aggregate, aggregate_columns))
    atomic_write(root / "ycsb-summary.json", json.dumps(public_summary, indent=2, sort_keys=True) + "\n")
    atomic_write(root / "ycsb-report.md", markdown(summary))


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("result_root", type=pathlib.Path)
    parser.add_argument("--validate-attempt", nargs=4, metavar=("WORKLOAD", "TRIAL", "MODE", "ATTEMPT"))
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    arguments = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if arguments.validate_attempt:
            workload, trial_text, mode, attempt_text = arguments.validate_attempt
            experiment = validate_experiment(json.loads((arguments.result_root / "experiment.json").read_text()))
            attempt = pathlib.Path(attempt_text)
            row = parse_attempt(arguments.result_root, experiment, workload, int(trial_text), mode, attempt)
            print(json.dumps(row, indent=2, sort_keys=True))
            return 0 if row["valid"] else 1
        summary = analyze(arguments.result_root)
        write_outputs(arguments.result_root, summary)
    except (AnalysisError, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0 if summary["invalid_rows"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
