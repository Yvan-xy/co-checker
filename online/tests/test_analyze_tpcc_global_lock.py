import csv
import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
import textwrap
import unittest
import urllib.parse
from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "tools" / "analyze_tpcc_global_lock.py"
RUNNER = Path(__file__).parents[1] / "scripts" / "run_tpcc_global_lock_comparison.sh"
SPEC = importlib.util.spec_from_file_location("analyze_tpcc_global_lock", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
analyzer = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = analyzer
SPEC.loader.exec_module(analyzer)


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract_shell_functions(first: str, following: str) -> str:
    source = RUNNER.read_text()
    start = source.index(f"{first}() {{")
    end = source.index(f"\n{following}() {{", start)
    return source[start:end]


def extract_python_heredoc(anchor: str) -> str:
    source = RUNNER.read_text()
    anchor_offset = source.index(anchor)
    start = source.index("<<'PY'\n", anchor_offset) + len("<<'PY'\n")
    end = source.index("\nPY\n", start)
    return source[start:end] + "\n"


def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False, **kwargs)


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n")


class TpccGlobalLockAnalyzerTest(unittest.TestCase):
    def create_experiment(self, root: Path, trials: int) -> None:
        write_json(
            root / "experiment.json",
            {
                "format_version": 2,
                "trials": trials,
                "smoke": trials < 5,
                "modes": list(analyzer.MODES),
                "mode_order": list(analyzer.MODES),
                "workload_label": "TPC-C full mix",
                "transaction_weights": [45, 43, 4, 4, 4],
                "workload": {
                    "prepare_threads": 1,
                    "go_tpc_ignore_error": False,
                    "go_tpc_silence_errors": True,
                    "go_tpc_error_policy": {
                        "policy": "continue-only-tidb-write-conflict",
                        "mysql_error_numbers": [9007],
                        "generic_ignore_error": False,
                    },
                },
                "preparation": {
                    "partition_type": {"code": 1, "name": "HASH"},
                    "partitions": 1,
                    "use_foreign_keys": False,
                    "initial_state": "fresh-empty-store-per-mode",
                },
                "storage_configuration": {
                    "coprocessor_cache_capacity_mb": 0,
                    "tidb_store_batch_size": 0,
                    "tidb_allow_batch_cop": False,
                    "tidb_index_lookup_pushdown_policy": "force",
                },
            },
        )

    def create_live_artifacts(
        self,
        attempt_dir: Path,
        mode: str,
        start: int,
        client_exit: int,
    ) -> None:
        sample_path = attempt_dir / "workload-checker-samples.jsonl"
        cutoff_path = attempt_dir / "workload-cutoff.json"
        resource_lines = ["monotonic_ns vmrss_kib vmhwm_kib process_cpu_ticks\n"]
        for index in range(102):
            timestamp = start - 500_000_000 + index * 1_000_000_000
            resource_lines.append(
                f"{timestamp} {1300 + index} {1500 + index} {1000 + index * 100}\n"
            )
        (attempt_dir / "tidb-resource-samples.txt").write_text(
            "".join(resource_lines)
        )
        if mode == "baseline-original":
            samples = [
                {
                    "format_version": 1,
                    "clock": "CLOCK_MONOTONIC",
                    "monotonic_ns": start - 500_000_000 + index * 1_000_000_000,
                    "checker_enabled": False,
                    "expected_mode": None,
                    "scrape_success": True,
                    "mode": None,
                    "ordering_protocol": None,
                    "adapter": {},
                    "native": {},
                    "verdict": None,
                    "error": None,
                }
                for index in range(102)
            ]
            sample_path.write_text("".join(json.dumps(sample) + "\n" for sample in samples))
            write_json(cutoff_path, {
                "format_version": 1,
                "checker": "disabled",
                "checker_enabled": False,
                "workload_cutoff_monotonic_ns": client_exit,
                "observation_monotonic_ns": client_exit + 50_000_000,
                "observation_delay_ns": 50_000_000,
            })
            return

        pipeline_mode = analyzer.PIPELINE_MODE[mode]
        target_name = {
            "capture-only": "recycled_transactions",
            "order-only": "ordered_transactions",
            "full": "checked_transactions",
        }[pipeline_mode]
        final_drain = json.loads((attempt_dir / "workload-drain.json").read_text())
        warmup_drain = json.loads(json.dumps(final_drain))
        warmup_drain["adapter_metrics"].update({
            "attempts_registered": 12,
            "attempts_published": 10,
            "attempts_aborted": 2,
            "successful_write_commits": 8,
            "read_only_no_write_completions": 2,
            "accepted_publications": 10,
            "publication_sequence": 10,
        })
        warmup_native = warmup_drain["native_metrics"]
        warmup_native.update({
            "captured_transactions": 12,
            "captured_events": 120,
            "accepted_publications": 10,
            "accepted_events": 100,
            "recycled_transactions": 10 if pipeline_mode == "capture-only" else 0,
            "ordered_transactions": 10 if pipeline_mode != "capture-only" else 0,
            "checked_transactions": 10 if pipeline_mode == "full" else 0,
            "checked_events": 100 if pipeline_mode == "full" else 0,
        })
        write_json(attempt_dir / "preparation-drain.json", warmup_drain)
        write_json(attempt_dir / "warmup-drain.json", warmup_drain)

        cutoff_native = dict(final_drain["native_metrics"])
        cutoff_native[target_name] = 99
        if pipeline_mode == "full":
            cutoff_native["checked_events"] = 990
        cutoff_adapter = dict(final_drain["adapter_metrics"])
        verdict = {
            "available": pipeline_mode == "full",
            "pass": pipeline_mode == "full",
            "violation": False,
        }
        cutoff = {
            "format_version": 1,
            "clock": "CLOCK_MONOTONIC",
            "checker_enabled": True,
            "mode": pipeline_mode,
            "ordering_protocol": "serialized-global-lock",
            "workload_cutoff_monotonic_ns": client_exit,
            "observation_monotonic_ns": client_exit + 50_000_000,
            "observation_delay_ns": 50_000_000,
            "adapter_metrics": cutoff_adapter,
            "native_metrics": cutoff_native,
            "verdict": verdict,
            "residual_transactions": 1,
            "residual_events": 10 if pipeline_mode == "full" else 0,
        }
        write_json(cutoff_path, cutoff)

        final_drain.update({
            "format_version": 2,
            "clock": "CLOCK_MONOTONIC",
            "workload_cutoff_monotonic_ns": client_exit,
            "drain_started_monotonic_ns": client_exit,
            "initial_scrape_monotonic_ns": client_exit + 50_000_000,
            "drain_complete_monotonic_ns": client_exit + 5_000_000_000,
            "live_prefix_verdict_monotonic_ns": (
                client_exit + 50_000_000 if pipeline_mode == "full" else None
            ),
            "cutoff_observation_delay_ns": 50_000_000,
            "initial_adapter_metrics": cutoff_adapter,
            "initial_native_metrics": cutoff_native,
            "initial_verdict": verdict,
            "cutoff_residual_transactions": 1,
            "cutoff_residual_events": 10 if pipeline_mode == "full" else 0,
            "drained_transactions": 1,
            "drained_events": 10 if pipeline_mode == "full" else 0,
            "drain_duration_seconds": 5.0,
        })
        write_json(attempt_dir / "workload-drain.json", final_drain)

        samples = []
        for index in range(102):
            timestamp = start - 500_000_000 + index * 1_000_000_000
            progress = min(100, max(10, 10 + int(index * 90 / 100)))
            target = progress if index < 100 else 99
            native = {
                "accepted_publications": progress,
                "accepted_events": progress * 10,
                "recycled_transactions": target if pipeline_mode == "capture-only" else 0,
                "ordered_transactions": target if pipeline_mode != "capture-only" else 0,
                "checked_transactions": target if pipeline_mode == "full" else 0,
                "checked_events": target * 10 if pipeline_mode == "full" else 0,
                "queue_depth": max(0, progress - target),
                "queue_bytes": max(0, progress - target) * 64,
            }
            samples.append({
                "format_version": 1,
                "clock": "CLOCK_MONOTONIC",
                "monotonic_ns": timestamp,
                "checker_enabled": True,
                "expected_mode": pipeline_mode,
                "scrape_success": True,
                "mode": pipeline_mode,
                "ordering_protocol": "serialized-global-lock",
                "adapter": {"accepted_publications": progress},
                "native": native,
                "verdict": verdict,
                "error": None,
            })
        sample_path.write_text("".join(json.dumps(sample) + "\n" for sample in samples))

    def create_attempt(
        self,
        root: Path,
        trial: int,
        mode: str,
        tpm_c: float,
        *,
        attempt: int = 1,
        fatal: bool = False,
    ) -> Path:
        attempt_dir = (
            root
            / "trials"
            / f"trial-{trial:03d}"
            / mode
            / f"attempt-{attempt:03d}"
        )
        attempt_dir.mkdir(parents=True)
        write_json(
            attempt_dir / "configuration.json",
            {
                "format_version": 2,
                "mode": mode,
                "workload_label": "TPC-C full mix",
                "workload": {
                    "weights": [45, 43, 4, 4, 4],
                    "prepare_threads": 1,
                    "go_tpc_ignore_error": False,
                    "go_tpc_silence_errors": True,
                    "go_tpc_error_policy": {
                        "policy": "continue-only-tidb-write-conflict",
                        "mysql_error_numbers": [9007],
                        "generic_ignore_error": False,
                    },
                },
                "preparation": {
                    "partition_type": {"code": 1, "name": "HASH"},
                    "partitions": 1,
                    "use_foreign_keys": False,
                },
                "storage_configuration": {
                    "fresh_store": True,
                    "coprocessor_cache_capacity_mb": 0,
                    "tidb_store_batch_size": 0,
                    "tidb_allow_batch_cop": False,
                    "tidb_index_lookup_pushdown_policy": "force",
                },
            },
        )
        write_json(
            attempt_dir / "runner-status.json",
            {"status": "complete", "go_tpc_pass": True},
        )
        write_json(attempt_dir / "completed.json", {"status": "complete"})
        for stage in ("prepare", "warmup", "workload"):
            validation = {"passed": True}
            if stage == "workload":
                validation["expected_seconds"] = 100.0
            write_json(attempt_dir / f"{stage}-validation.json", validation)
            write_json(attempt_dir / f"{stage}-status.json", {"exit_status": 0})
        write_json(attempt_dir / "schema-bootstrap-validation.json", {"passed": True})
        write_json(attempt_dir / "prepare-manifest.json", {"format_version": 1})
        write_json(attempt_dir / "schema-bootstrap-digest.json", {"format_version": 2})
        write_json(attempt_dir / "post-prepare-digest.json", {"format_version": 2})
        preparation = {
            "format_version": 1,
            "prepare_seed": 20260804,
            "prepare_load_time": "2024-01-01 00:00:00",
            "worker_count": 1,
            "manifest_file_sha256": "a" * 64,
            "ordered_manifest_sha256": "b" * 64,
            "transaction_count": 9,
            "logical_write_count": 100,
            "schema_digest_sha256": "c" * 64,
            "data_digest_sha256": "d" * 64,
            "database_digest_sha256": "e" * 64,
            "total_rows": 100,
            "table_row_counts": {"warehouse": 1, "item": 99},
        }
        write_json(
            attempt_dir / "prepare-reference-validation.json",
            {"passed": True, "observed": preparation, "reference": preparation},
        )
        if mode == "baseline-original":
            drain = {
                "passed": True,
                "drained": True,
                "mode": mode,
                "checker": "disabled",
            }
        else:
            pipeline_mode = analyzer.PIPELINE_MODE[mode]
            drain = {
                "passed": True,
                "drained": True,
                "mode": pipeline_mode,
                "live_scrape_success": True,
                "adapter_metrics": {
                    "active_attempts": 0,
                    "active_finalizers": 0,
                    "active_fragments": 0,
                    "fragment_buffer_active_bytes": 0,
                    "fragment_retained_active_bytes": 0,
                    "fatal_calls": 0,
                    "fragment_capture_errors": 0,
                    "fragments_started": 10,
                    "fragments_accepted": 8,
                    "fragments_discarded": 2,
                    "attempts_registered": 102,
                    "attempts_published": 100,
                    "attempts_aborted": 2,
                    "successful_write_commits": 80,
                    "read_only_no_write_completions": 20,
                    "accepted_publications": 100,
                    "publication_sequence": 100,
                },
                "native_metrics": {
                    "fatal": 0,
                    "active_attempts": 0,
                    "queue_reserved_bytes": 0,
                    "queue_reserved_depth": 0,
                    "queue_bytes": 0,
                    "queue_depth": 0,
                    "reorder_bytes": 0,
                    "captured_transactions": 102,
                    "captured_events": 1020,
                    "accepted_publications": 100,
                    "accepted_events": 1000,
                    "ordered_transactions": 0 if pipeline_mode == "capture-only" else 100,
                    "checked_transactions": 100 if pipeline_mode == "full" else 0,
                    "checked_events": 1000 if pipeline_mode == "full" else 0,
                    "verdict_available": 1 if pipeline_mode == "full" else 0,
                    "verdict_pass": 1 if pipeline_mode == "full" else 0,
                    "recycled_transactions": 100 if pipeline_mode == "capture-only" else 0,
                    "verdict_violation": 0,
                },
            }
        for filename in (
            "preparation-drain.json", "warmup-drain.json", "workload-drain.json"
        ):
            write_json(attempt_dir / filename, drain)

        successes = int(tpm_c * 100.0 / 60.0)
        workload_records = []
        counts = {
            "NEW_ORDER": successes,
            "PAYMENT": max(1, int(successes * 43 / 45)),
            "ORDER_STATUS": max(1, int(successes * 4 / 45)),
            "DELIVERY": max(1, int(successes * 4 / 45)),
            "STOCK_LEVEL": max(1, int(successes * 4 / 45)),
        }
        for index, (_, operation) in enumerate(analyzer.TRANSACTIONS):
            workload_records.extend(
                (
                    {
                        "Prefix": "[Summary]",
                        "Operation": operation,
                        "Count": counts[operation],
                        "Takes(s)": 100.0,
                        "TPM": tpm_c,
                        "99th(ms)": 25.0 + trial + index,
                    },
                    {
                        "Prefix": "[Summary]",
                        "Operation": operation + "_ERR",
                        "Count": 0,
                    },
                )
            )
        (attempt_dir / "workload.txt").write_text(
            json.dumps(workload_records)
            + "\n"
            + json.dumps({"tpmC": tpm_c, "tpmTotal": tpm_c})
            + "\n"
            + json.dumps(
                {
                    "format_version": 1,
                    "record": "go-tpc-run-error-policy",
                    "policy": "continue-only-tidb-write-conflict",
                    "mysql_error_number": 9007,
                    "continued_count": 0,
                    "rejected_count": 0,
                }
            )
            + "\n"
            + "\nFinished\n"
        )
        (attempt_dir / "tidb-pidstat.txt").write_text(
            "# Time PID %CPU RSS Command\n"
            "1 123 100.0 1200 tidb-server\n"
            "2 123 120.0 1400 tidb-server\n"
        )
        (attempt_dir / "tidb-proc-after.txt").write_text(
            "VmRSS:\t1300 kB\nVmHWM:\t1500 kB\n"
        )

        start = 1_000_000_000_000 + trial * 1_000_000_000
        client_exit = start + 101_250_000_000
        timing = {
            "workload_start_monotonic_ns": start,
            "workload_client_exit_monotonic_ns": client_exit,
            "server_shutdown_start_monotonic_ns": client_exit + 5_000_000_000,
            "tidb_server_exit_monotonic_ns": client_exit + 6_000_000_000,
            # This huge post-check timestamp must not enter verdict timing.
            "tpcc_post_check_exit_monotonic_ns": client_exit + 10_000_000_000_000,
        }
        if mode != "baseline-original":
            completion = client_exit + 5_000_000_000
            timing.update({
                "checker_result_available_monotonic_ns": completion,
                "checker_drain_started_monotonic_ns": client_exit,
                "checker_drain_complete_monotonic_ns": completion,
                "checker_completion_monotonic_ns": completion,
                "live_prefix_verdict_monotonic_ns": (
                    client_exit + 50_000_000 if mode == "serialized-full" else None
                ),
            })
        write_json(attempt_dir / "timing.json", timing)
        self.create_live_artifacts(attempt_dir, mode, start, client_exit)
        write_json(attempt_dir / "post-check-validation.json", {"passed": True})
        write_json(
            attempt_dir / "server-log-validation.json",
            {
                "format_version": 1,
                "mode": mode,
                "passed": True,
                "files": [
                    {"path": name, "bytes": 1, "lines": 1}
                    for name in (
                        "schema-bootstrap-server.txt",
                        "schema-bootstrap-server-stdout.txt",
                        "main-server.txt",
                        "main-server-stdout.txt",
                        "post-check-server.txt",
                        "post-check-server-stdout.txt",
                    )
                ],
                "failure_matches": [],
            },
        )

        if mode != "baseline-original":
            ordered_final = 0 if mode == "serialized-capture-only" else 100
            checked_final = 100 if mode == "serialized-full" else 0
            checked_events = 1000 if mode == "serialized-full" else 0
            pre_ordered = 0 if mode == "serialized-capture-only" else 98
            pre_checked = 97 if mode == "serialized-full" else 0
            result = {
                "format_version": 1,
                "mode": analyzer.PIPELINE_MODE[mode],
                "ordering_protocol": "serialized-global-lock",
                "outcome": analyzer.EXPECTED_OUTCOME[mode],
                "close_status": 0,
                "native_close_nanoseconds": 2_000_000_000,
                "frontier_metrics": {
                    "pump_attempts": 0,
                    "pump_successes": 0,
                    "pump_errors": 0,
                },
                "adapter_metrics": {
                    "attempts_registered": 102,
                    "attempts_published": 100,
                    "attempts_aborted": 2,
                    "fatal_calls": 0,
                    "publish_errors": 0,
                    "abort_errors": 0,
                    "captured_events": 1020,
                    "active_attempts": 0,
                    "fragments_started": 10,
                    "fragments_storage_attached": 10,
                    "fragments_accepted": 8,
                    "fragments_discarded": 2,
                    "active_fragments": 0,
                    "fragment_buffer_active_bytes": 0,
                    "fragment_retained_active_bytes": 0,
                    "fragments_late_discarded": 1,
                    "fragment_late_discarded_events": 2,
                    "fragment_late_discarded_bytes": 20,
                    "fragment_discarded_events": 4,
                    "fragment_discarded_payload_bytes": 40,
                    "fragment_capture_errors": 0,
                    "finalization_acquisitions": 100,
                    "active_finalizers": 0,
                    "max_active_finalizers": 8,
                    "finalization_wait_nanos": 1000,
                    "max_finalization_wait_nanos": 50,
                    "finalization_hold_nanos": 2000,
                    "max_finalization_hold_nanos": 100,
                    "successful_write_commits": 80,
                    "read_only_no_write_completions": 20,
                    "accepted_publications": 100,
                    "publication_sequence": 100,
                },
                "native_metrics_before_close": {
                    "captured_transactions": 102,
                    "accepted_publications": 100,
                    "ordered_transactions": pre_ordered,
                    "checked_transactions": pre_checked,
                    "queue_depth": 2,
                },
                "native_metrics": {
                    "mode": analyzer.NATIVE_MODE[mode],
                    "ordering_protocol": 1,
                    "closed": True,
                    "fatal": fatal,
                    "active_attempts": 0,
                    "captured_transactions": 102,
                    "captured_events": 1020,
                    "captured_used_bytes": 10_000 + trial * 100,
                    "captured_reserved_bytes": 12_000 + trial * 100,
                    "accepted_publications": 100,
                    "accepted_events": 1000,
                    "ordered_transactions": ordered_final,
                    "checked_transactions": checked_final,
                    "checked_events": checked_events,
                    "recycled_transactions": (
                        100 if mode == "serialized-capture-only" else 0
                    ),
                    "violating_transactions": 0,
                    "committed_but_unpublished": 0,
                    "capture_active_bytes": 0,
                    "queue_depth": 0,
                    "queue_bytes": 0,
                    "queue_reserved_depth": 0,
                    "queue_reserved_bytes": 0,
                    "reorder_bytes": 0,
                    "reorder_capacity_bytes": 0,
                    "reorder_peak_bytes": 0,
                    "queue_peak_depth": 7,
                    "queue_peak_bytes": 4096,
                    "queue_backpressure_wait_count": 3,
                    "queue_backpressure_ns": 3000,
                    "total_queue_lag_ns": 10_000,
                    "max_queue_lag_ns": 500,
                    "queue_residence_sample_count": 100,
                },
            }
            if mode == "serialized-full":
                result["verdict"] = {"status": "pass"}
            write_json(attempt_dir / "checker-result.json", result)
        return attempt_dir

    def test_paired_summary_retains_invalid_run_and_excludes_post_check_time(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_experiment(root, 2)
            tpm_by_mode = {
                "baseline-original": (600.0, 660.0),
                "serialized-capture-only": (540.0, 594.0),
                "serialized-order-only": (480.0, 528.0),
                "serialized-full": (450.0, 462.0),
            }
            for mode, values in tpm_by_mode.items():
                for trial, tpm_c in enumerate(values, start=1):
                    self.create_attempt(
                        root,
                        trial,
                        mode,
                        tpm_c,
                        fatal=(trial == 2 and mode == "serialized-order-only"),
                    )

            # A newer incomplete retry must not displace a valid completed
            # attempt in the append-only --resume layout.
            retry = (
                root
                / "trials"
                / "trial-001"
                / "serialized-capture-only"
                / "attempt-002"
            )
            retry.mkdir()
            write_json(retry / "configuration.json", {"incomplete": True})

            result = analyzer.analyze(root)
            analyzer.write_outputs(root, result)

            self.assertFalse(result["all_valid"])
            self.assertEqual(len(result["trial_rows"]), 8)
            self.assertEqual(result["invalid_run_count"], 1)
            rows = {
                (row["trial_index"], row["mode"]): row
                for row in result["trial_rows"]
            }
            self.assertEqual(
                rows[(1, "serialized-capture-only")]["attempt"], "attempt-001"
            )
            self.assertTrue(rows[(1, "serialized-capture-only")]["valid"])
            invalid_order = rows[(2, "serialized-order-only")]
            self.assertFalse(invalid_order["valid"])
            self.assertIn("fatal", invalid_order["invalid_reason"])
            self.assertEqual(
                rows[(1, "serialized-full")]["throughput_overhead_percent"], 25.0
            )
            self.assertEqual(
                rows[(2, "serialized-full")]["throughput_overhead_percent"], 30.0
            )
            full = rows[(1, "serialized-full")]
            self.assertAlmostEqual(
                full["fixed_window_new_order_transactions_per_minute"],
                750.0 * 60.0 / 101.25,
            )
            self.assertAlmostEqual(
                full["end_to_verdict_new_order_transactions_per_minute"],
                750.0 * 60.0 / 106.25,
            )
            self.assertAlmostEqual(
                full[
                    "paired_baseline_fixed_window_new_order_transactions_per_minute"
                ],
                1000.0 * 60.0 / 101.25,
            )
            self.assertEqual(
                full["fixed_window_throughput_overhead_percent"], 25.0
            )
            self.assertAlmostEqual(
                full["end_to_verdict_throughput_overhead_percent"],
                (
                    1000.0 * 60.0 / 101.25
                    - 750.0 * 60.0 / 106.25
                ) * 100.0 / (1000.0 * 60.0 / 101.25),
            )
            self.assertIsNone(
                rows[(1, "baseline-original")]["end_to_verdict_new_order_transactions_per_minute"]
            )
            self.assertEqual(full["workload_start_to_verdict_seconds"], 106.25)
            self.assertEqual(full["post_workload_drain_to_verdict_seconds"], 5.0)
            self.assertEqual(
                full["checker_result_available_monotonic_ns"],
                full["checker_completion_monotonic_ns"],
            )
            self.assertLess(full["live_prefix_verdict_monotonic_ns"], full["checker_result_available_monotonic_ns"])
            self.assertIsNone(
                rows[(1, "baseline-original")]["workload_start_to_verdict_seconds"]
            )
            self.assertEqual(full["captured_minus_ordered_transaction_tail"], 4)
            self.assertEqual(
                full["accepted_publications_minus_ordered_transaction_tail"], 2
            )
            self.assertEqual(full["queue_average_residence_time_ns"], 100.0)
            self.assertEqual(full["captured_used_bytes"], 10_100)
            self.assertEqual(full["captured_reserved_bytes"], 12_100)
            # Backpressure is a reported metric, not an invalidity condition.
            self.assertTrue(full["valid"])

            self.assertTrue(full["live_sustainability_pass"])
            self.assertEqual(full["checker_sample_coverage_percent"], 100.0)
            self.assertEqual(full["live_stalled_window_count"], 0)
            self.assertEqual(full["cutoff_residual_transactions"], 1)
            self.assertEqual(full["drained_transactions"], 1)
            self.assertEqual(full["tidb_workload_cpu_seconds"], 100.0)
            self.assertEqual(full["tidb_max_rss_kib"], 1401)
            self.assertEqual(
                rows[(1, "baseline-original")]["tidb_workload_cpu_seconds"], 100.0
            )
            self.assertEqual(full["checker_drain_duration_seconds"], 5.0)
            self.assertLessEqual(full["checker_result_available_monotonic_ns"], full["server_shutdown_start_monotonic_ns"])
            summaries = {row["mode"]: row for row in result["summary_rows"]}
            full_summary = summaries["serialized-full"]
            self.assertEqual(
                full_summary["throughput_overhead_percent_median"], 27.5
            )
            self.assertEqual(
                full_summary["throughput_overhead_percent_minimum"], 25.0
            )
            self.assertEqual(
                full_summary["throughput_overhead_percent_maximum"], 30.0
            )
            self.assertEqual(full_summary["captured_events_median"], 1020.0)
            self.assertEqual(
                full_summary[
                    "fixed_window_throughput_overhead_percent_median"
                ],
                27.5,
            )
            self.assertAlmostEqual(
                full_summary[
                    "end_to_verdict_throughput_overhead_percent_median"
                ],
                30.911764705882355,
            )
            self.assertAlmostEqual(
                full_summary[
                    "end_to_verdict_throughput_overhead_percent_minimum"
                ],
                28.529411764705888,
            )
            self.assertAlmostEqual(
                full_summary[
                    "end_to_verdict_throughput_overhead_percent_maximum"
                ],
                33.294117647058826,
            )
            self.assertEqual(full_summary["captured_used_bytes_median"], 10_150.0)
            self.assertEqual(
                full_summary["captured_reserved_bytes_median"], 12_150.0
            )
            self.assertEqual(
                summaries["serialized-order-only"]["valid_trial_count"], 1
            )
            self.assertEqual(
                summaries["serialized-order-only"]["invalid_trial_count"], 1
            )

            expected_outputs = {
                "tpcc-trials.csv",
                "tpcc-performance.csv",
                "tpcc-summary.csv",
                "tpcc-summary.json",
                "tpcc-report.md",
            }
            self.assertTrue(
                expected_outputs.issubset(path.name for path in root.iterdir())
            )
            with (root / "tpcc-trials.csv").open(newline="") as stream:
                csv_rows = list(csv.DictReader(stream))
            self.assertEqual(len(csv_rows), 8)
            self.assertEqual(set(csv_rows[0]), set(analyzer.TRIAL_COLUMNS))
            with (root / "tpcc-performance.csv").open(newline="") as stream:
                performance_rows = list(csv.DictReader(stream))
            self.assertEqual(len(performance_rows), 4)
            self.assertEqual(
                tuple(performance_rows[0]),
                analyzer.PERFORMANCE_COLUMNS,
            )
            performance_by_mode = {
                row["mode"]: row for row in performance_rows
            }
            performance_full = performance_by_mode["serialized-full"]
            self.assertEqual(int(performance_full["valid_trial_count"]), 2)
            self.assertAlmostEqual(
                float(
                    performance_full[
                        "end_to_verdict_throughput_overhead_percent_median"
                    ]
                ),
                30.911764705882355,
            )
            performance_baseline = performance_by_mode["baseline-original"]
            self.assertAlmostEqual(
                float(
                    performance_baseline[
                        "end_to_verdict_new_order_transactions_per_minute_median"
                    ]
                ),
                summaries["baseline-original"][
                    "fixed_window_new_order_transactions_per_minute_median"
                ],
            )
            self.assertEqual(
                float(
                    performance_baseline[
                        "end_to_verdict_throughput_overhead_percent_median"
                    ]
                ),
                0.0,
            )
            summary_json = json.loads((root / "tpcc-summary.json").read_text())
            self.assertFalse(summary_json["all_valid"])
            report = (root / "tpcc-report.md").read_text()
            self.assertIn("TPC-C full-mix", report)
            self.assertIn("Invalid runs", report)
            self.assertIn(analyzer.CHECKER_METRIC_SCOPE, report)
            self.assertIn("Queue peak d/B", report)
            self.assertIn("Captured ev/U/R B", report)
            self.assertIn("Native close", report)
            report_lines = report.splitlines()
            self.assertIn("End-to-verdict NO/min", report)
            self.assertIn("Fixed OH", report)
            for header in ("| Trial | Mode |", "| Mode | Valid/invalid |"):
                index = next(
                    i for i, line in enumerate(report_lines) if line.startswith(header)
                )
                self.assertEqual(
                    report_lines[index].count("|"),
                    report_lines[index + 1].count("|"),
                )

    def test_parse_workload_requires_all_five_summaries_and_reports_errors(self) -> None:
        def records(error_operation: str | None = None, omit: str | None = None) -> str:
            values = []
            for _, operation in analyzer.TRANSACTIONS:
                if operation == omit:
                    continue
                values.extend((
                    {
                        "Prefix": "[Summary]",
                        "Operation": operation,
                        "Count": 10,
                        "Takes(s)": 10.0,
                        "99th(ms)": 2.0,
                    },
                    {
                        "Prefix": "[Summary]",
                        "Operation": operation + "_ERR",
                        "Count": 1 if operation == error_operation else 0,
                    },
                ))
            policy = {
                "format_version": 1,
                "record": "go-tpc-run-error-policy",
                "policy": "continue-only-tidb-write-conflict",
                "mysql_error_number": 9007,
                "continued_count": 1 if error_operation is not None else 0,
                "rejected_count": 0,
            }
            return (
                json.dumps(values) + "\n" + json.dumps({"tpmC": 100})
                + "\n" + json.dumps(policy) + "\nFinished\n"
            )

        with tempfile.TemporaryDirectory() as directory:
            workload = Path(directory) / "workload.txt"
            workload.write_text(records())
            reasons: list[str] = []
            parsed = analyzer.parse_workload(workload, reasons)
            self.assertFalse(reasons)
            self.assertEqual(len(parsed["transactions"]), 5)

            workload.write_text(records(error_operation="PAYMENT"))
            reasons = []
            parsed = analyzer.parse_workload(workload, reasons)
            self.assertFalse(reasons)
            self.assertEqual(parsed["transactions"]["payment"]["error_count"], 1)
            mismatched = records(error_operation="PAYMENT").replace(
                '"continued_count": 1', '"continued_count": 2'
            )
            workload.write_text(mismatched)
            reasons = []
            analyzer.parse_workload(workload, reasons)
            self.assertTrue(any("do not conserve" in reason for reason in reasons))


            workload.write_text(records(omit="STOCK_LEVEL"))
            reasons = []
            analyzer.parse_workload(workload, reasons)
            self.assertTrue(any("STOCK_LEVEL summary" in reason for reason in reasons))

            lines = records().splitlines()
            new_order_records = [
                record
                for record in json.loads(lines[0])
                if record["Operation"] in ("NEW_ORDER", "NEW_ORDER_ERR")
            ]
            workload.write_text(
                json.dumps(new_order_records) + "\n" + "\n".join(lines[1:]) + "\n"
            )
            reasons = []
            parsed = analyzer.parse_workload(
                workload,
                reasons,
                (100, 0, 0, 0, 0),
            )
            self.assertFalse(reasons)
            self.assertEqual(len(parsed["transactions"]), 5)
            self.assertEqual(parsed["transactions"]["payment"]["success_count"], 0)

    def test_baseline_full_only_matrix_is_paired_and_complete(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_experiment(root, 2)
            experiment_path = root / "experiment.json"
            experiment = json.loads(experiment_path.read_text())
            experiment["modes"] = list(analyzer.BASELINE_FULL_MODES)
            experiment["mode_order"] = list(analyzer.BASELINE_FULL_MODES)
            write_json(experiment_path, experiment)

            for trial in (1, 2):
                self.create_attempt(
                    root, trial, "baseline-original", 500.0 + trial
                )
                self.create_attempt(
                    root, trial, "serialized-full", 400.0 + trial
                )

            result = analyzer.analyze(root)
            self.assertTrue(result["all_valid"])
            self.assertEqual(result["modes"], list(analyzer.BASELINE_FULL_MODES))
            self.assertEqual(len(result["trial_rows"]), 4)
            self.assertEqual(
                [row["mode"] for row in result["summary_rows"]],
                list(analyzer.BASELINE_FULL_MODES),
            )
            rows = {
                (row["trial_index"], row["mode"]): row
                for row in result["trial_rows"]
            }
            full = rows[(1, "serialized-full")]
            self.assertAlmostEqual(
                full["throughput_overhead_percent"],
                (501.0 - 401.0) * 100.0 / 501.0,
            )
            self.assertGreater(
                full["end_to_verdict_throughput_overhead_percent"],
                full["fixed_window_throughput_overhead_percent"],
            )
            self.assertEqual(
                full["checker_completion_monotonic_ns"],
                full["checker_drain_complete_monotonic_ns"],
            )

    def test_analyzer_rejects_noncanonical_mode_matrices(self) -> None:
        invalid_matrices = (
            ["serialized-full", "baseline-original"],
            ["baseline-original", "baseline-original", "serialized-full"],
            ["baseline-original"],
            ["baseline-original", "serialized-order-only"],
        )
        for modes in invalid_matrices:
            with self.subTest(modes=modes), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self.create_experiment(root, 1)
                experiment_path = root / "experiment.json"
                experiment = json.loads(experiment_path.read_text())
                experiment["modes"] = modes
                experiment["mode_order"] = modes
                write_json(experiment_path, experiment)
                with self.assertRaisesRegex(
                    analyzer.AnalysisError, "supported mode matrix"
                ):
                    analyzer.analyze(root)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_experiment(root, 1)
            experiment_path = root / "experiment.json"
            experiment = json.loads(experiment_path.read_text())
            experiment["modes"] = list(analyzer.BASELINE_FULL_MODES)
            experiment["mode_order"] = list(reversed(analyzer.BASELINE_FULL_MODES))
            write_json(experiment_path, experiment)
            with self.assertRaisesRegex(analyzer.AnalysisError, "mode_order"):
                analyzer.analyze(root)

    def test_analyzer_allows_nondefault_weights_only_for_smoke(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_experiment(root, 1)
            experiment_path = root / "experiment.json"
            experiment = json.loads(experiment_path.read_text())
            experiment["transaction_weights"] = [100, 0, 0, 0, 0]
            write_json(experiment_path, experiment)

            result = analyzer.analyze(root)
            self.assertEqual(result["transaction_weights"], [100, 0, 0, 0, 0])
            self.assertFalse(result["all_valid"])

            experiment["smoke"] = False
            write_json(experiment_path, experiment)
            with self.assertRaisesRegex(
                analyzer.AnalysisError,
                "only for smoke diagnostics",
            ):
                analyzer.analyze(root)

            experiment["smoke"] = True
            experiment["transaction_weights"] = [0, 100, 0, 0, 0]
            write_json(experiment_path, experiment)
            with self.assertRaisesRegex(analyzer.AnalysisError, "positive NewOrder"):
                analyzer.analyze(root)

    def test_runner_sustains_workers_and_accounts_structured_errors(self) -> None:
        validator = extract_python_heredoc("validate_go_tpc_run() {")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            workload = root / "workload.txt"
            validation = root / "workload-validation.json"
            records = []
            for _, operation in analyzer.TRANSACTIONS:
                records.extend((
                    {
                        "Prefix": "[Current]",
                        "Operation": operation,
                        "Count": "5",
                        "Takes(s)": "1.0",
                    },
                    {
                        "Prefix": "[Summary]",
                        "Operation": operation,
                        "Count": "10",
                        "Takes(s)": "2.0",
                        "99th(ms)": "2.0",
                    },
                    {
                        "Prefix": "[Summary]",
                        "Operation": operation + "_ERR",
                        "Count": "1" if operation == "PAYMENT" else "0",
                    },
                ))
            workload.write_text(
                json.dumps(records)
                + "\n"
                + json.dumps(
                    {
                        "format_version": 1,
                        "record": "go-tpc-run-error-policy",
                        "policy": "continue-only-tidb-write-conflict",
                        "mysql_error_number": 9007,
                        "continued_count": 1,
                        "rejected_count": 0,
                    }
                )
                + "\n"
                + json.dumps({"tpmC": "300.0"})
                + "\nFinished\n"
            )
            completed = run([
                sys.executable,
                "-c",
                validator,
                "2s",
                "1s",
                "0",
                str(workload),
                str(validation),
                "45,43,4,4,4",
                "1000000000",
                "3000000000",
            ])
            self.assertEqual(completed.returncode, 0, completed.stderr)
            result = json.loads(validation.read_text())
            self.assertTrue(result["passed"])
            self.assertFalse(result["go_tpc_ignore_error"])
            self.assertTrue(result["go_tpc_silence_errors"])
            self.assertEqual(result["continued_write_conflict_count"], 1)
            self.assertEqual(result["transactions"]["payment"]["error_count"], 1)

            workload.write_text(workload.read_text() + "fatal error: injected\n")
            failed = run([
                sys.executable,
                "-c",
                validator,
                "2s",
                "1s",
                "0",
                str(workload),
                str(root / "failed-validation.json"),
                "45,43,4,4,4",
                "1000000000",
                "3000000000",
            ])
            self.assertNotEqual(failed.returncode, 0)

            new_order_records = [
                record
                for record in records
                if record.get("Operation") in ("NEW_ORDER", "NEW_ORDER_ERR")
            ]
            new_order_records.append(
                {
                    "format_version": 1,
                    "record": "go-tpc-run-error-policy",
                    "policy": "continue-only-tidb-write-conflict",
                    "mysql_error_number": 9007,
                    "continued_count": 0,
                    "rejected_count": 0,
                }
            )
            workload.write_text(
                json.dumps(new_order_records)
                + "\n"
                + json.dumps({"tpmC": "300.0"})
                + "\nFinished\n"
            )
            new_order_only_validation = root / "new-order-only-validation.json"
            new_order_only = run([
                sys.executable,
                "-c",
                validator,
                "2s",
                "1s",
                "0",
                str(workload),
                str(new_order_only_validation),
                "100,0,0,0,0",
                "1000000000",
                "3000000000",
            ])
            self.assertEqual(new_order_only.returncode, 0, new_order_only.stderr)
            diagnostic = json.loads(new_order_only_validation.read_text())
            self.assertEqual(diagnostic["transaction_weights"], [100, 0, 0, 0, 0])
            self.assertEqual(diagnostic["transactions"]["payment"]["count"], 0)

    def test_live_sustainability_fails_closed(self) -> None:

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            attempt = self.create_attempt(root, 1, "serialized-full", 500.0)

            samples_path = attempt / "workload-checker-samples.jsonl"
            samples = [json.loads(line) for line in samples_path.read_text().splitlines()]
            for sample in samples:
                sample["native"]["checked_transactions"] = 10
                sample["native"]["checked_events"] = 100
            samples_path.write_text(
                "".join(json.dumps(sample) + "\n" for sample in samples)
            )
            row = analyzer.parse_attempt(
                root, attempt, 1, "serialized-full", "TPC-C full mix"
            )
            self.assertFalse(row["valid"])
            self.assertIn("stalled", row["invalid_reason"])

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            attempt = self.create_attempt(root, 1, "serialized-full", 500.0)
            samples_path = attempt / "workload-checker-samples.jsonl"
            samples = samples_path.read_text().splitlines()
            samples_path.write_text("\n".join(samples[::2]) + "\n")
            row = analyzer.parse_attempt(
                root, attempt, 1, "serialized-full", "TPC-C full mix"
            )
            self.assertFalse(row["valid"])
            self.assertIn("sample coverage", row["invalid_reason"])

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            attempt = self.create_attempt(root, 1, "serialized-full", 500.0)
            cutoff_path = attempt / "workload-cutoff.json"
            cutoff = json.loads(cutoff_path.read_text())
            cutoff["native_metrics"]["checked_transactions"] = 90
            cutoff["native_metrics"]["checked_events"] = 900
            cutoff["residual_transactions"] = 10
            cutoff["residual_events"] = 100
            write_json(cutoff_path, cutoff)
            drain_path = attempt / "workload-drain.json"
            drain = json.loads(drain_path.read_text())
            timing_path = attempt / "timing.json"
            timing = json.loads(timing_path.read_text())
            client_exit = timing["workload_client_exit_monotonic_ns"]
            completion = client_exit + 45_000_000_000
            timing.update({
                "checker_result_available_monotonic_ns": completion,
                "checker_drain_complete_monotonic_ns": completion,
                "checker_completion_monotonic_ns": completion,
                "server_shutdown_start_monotonic_ns": completion + 1_000_000_000,
                "tidb_server_exit_monotonic_ns": completion + 2_000_000_000,
            })
            write_json(timing_path, timing)
            drain["drain_complete_monotonic_ns"] = completion
            drain["drain_duration_seconds"] = 45.0
            drain["initial_native_metrics"]["checked_transactions"] = 90
            drain["initial_native_metrics"]["checked_events"] = 900
            drain["cutoff_residual_transactions"] = 10
            drain["cutoff_residual_events"] = 100
            drain["drained_transactions"] = 10
            drain["drained_events"] = 100
            write_json(drain_path, drain)
            row = analyzer.parse_attempt(
                root, attempt, 1, "serialized-full", "TPC-C full mix"
            )
            self.assertTrue(row["valid"], row["invalid_reason"])
            self.assertFalse(row["live_sustainability_pass"])

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            attempt = self.create_attempt(root, 1, "serialized-full", 500.0)
            drain_path = attempt / "workload-drain.json"
            drain = json.loads(drain_path.read_text())
            drain["native_metrics"]["accepted_publications"] = 101
            write_json(drain_path, drain)
            row = analyzer.parse_attempt(
                root, attempt, 1, "serialized-full", "TPC-C full mix"
            )
            self.assertFalse(row["valid"])
            self.assertIn("cutoff barrier", row["invalid_reason"])

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            attempt = self.create_attempt(root, 1, "serialized-full", 500.0)
            drain_path = attempt / "workload-drain.json"
            drain = json.loads(drain_path.read_text())
            drain["native_metrics"]["verdict_available"] = 0
            write_json(drain_path, drain)
            row = analyzer.parse_attempt(
                root, attempt, 1, "serialized-full", "TPC-C full mix"
            )
            self.assertFalse(row["valid"])
            self.assertIn("live verdict is unavailable", row["invalid_reason"])

    def test_live_drain_requires_zero_fragment_ownership_and_mode_progress(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            attempt = self.create_attempt(
                root, 1, "serialized-order-only", 500.0
            )
            record = json.loads((attempt / "workload-drain.json").read_text())
            reasons: list[str] = []
            self.assertTrue(
                analyzer.validate_live_drain(
                    record, reasons, "serialized-order-only", "drain"
                )
            )
            self.assertFalse(reasons)

            record["adapter_metrics"]["active_fragments"] = 1
            reasons = []
            self.assertFalse(
                analyzer.validate_live_drain(
                    record, reasons, "serialized-order-only", "drain"
                )
            )
            self.assertTrue(any("active_fragments" in reason for reason in reasons))

            record["adapter_metrics"]["active_fragments"] = 0
            record["native_metrics"]["ordered_transactions"] = 99
            reasons = []
            self.assertFalse(
                analyzer.validate_live_drain(
                    record, reasons, "serialized-order-only", "drain"
                )
            )
            self.assertTrue(any("ordering lifecycle" in reason for reason in reasons))

    def test_preparation_digest_mismatch_and_checker_progress_are_invalid(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            attempt = self.create_attempt(root, 1, "serialized-full", 500.0)
            validation_path = attempt / "prepare-reference-validation.json"
            validation = json.loads(validation_path.read_text())
            validation["reference"] = dict(validation["reference"])
            validation["reference"]["database_digest_sha256"] = "f" * 64
            write_json(validation_path, validation)
            row = analyzer.parse_attempt(
                root, attempt, 1, "serialized-full", "TPC-C full mix"
            )
            self.assertFalse(row["valid"])
            self.assertIn("does not exactly match", row["invalid_reason"])

            validation["reference"] = validation["observed"]
            write_json(validation_path, validation)
            checker_path = attempt / "checker-result.json"
            checker = json.loads(checker_path.read_text())
            checker["native_metrics"]["checked_transactions"] = 99
            write_json(checker_path, checker)
            row = analyzer.parse_attempt(
                root, attempt, 1, "serialized-full", "TPC-C full mix"
            )
            self.assertFalse(row["valid"])
            self.assertIn("checked transactions", row["invalid_reason"])

    def test_preparation_partition_and_foreign_key_settings_fail_closed(self) -> None:
        cases = (
            ("partition_type", {"code": 2, "name": "RANGE"}, "partition type"),
            ("partitions", 2, "partition count"),
            ("use_foreign_keys", True, "foreign keys"),
        )
        for field, value, message in cases:
            with self.subTest(scope="experiment", field=field):
                with tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    self.create_experiment(root, 1)
                    experiment_path = root / "experiment.json"
                    experiment = json.loads(experiment_path.read_text())
                    experiment["preparation"][field] = value
                    write_json(experiment_path, experiment)
                    with self.assertRaisesRegex(analyzer.AnalysisError, message):
                        analyzer.analyze(root)

        for field, value, message in cases:
            with self.subTest(scope="attempt", field=field):
                with tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    attempt = self.create_attempt(
                        root, 1, "serialized-capture-only", 500.0
                    )
                    configuration_path = attempt / "configuration.json"
                    configuration = json.loads(configuration_path.read_text())
                    configuration["preparation"][field] = value
                    write_json(configuration_path, configuration)
                    row = analyzer.parse_attempt(
                        root,
                        attempt,
                        1,
                        "serialized-capture-only",
                        "TPC-C full mix",
                    )
                    self.assertFalse(row["valid"])
                    self.assertIn(message, row["invalid_reason"])

    def test_native_capture_byte_counters_fail_closed(self) -> None:
        cases = (
            ("captured_used_bytes", None, "captured_used_bytes"),
            ("captured_used_bytes", 13_000, "smaller than captured_used_bytes"),
            ("captured_reserved_bytes", 0, "captured_reserved_bytes did not advance"),
        )
        for field, value, message in cases:
            with self.subTest(field=field, value=value):
                with tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    attempt = self.create_attempt(root, 1, "serialized-full", 500.0)
                    checker_path = attempt / "checker-result.json"
                    checker = json.loads(checker_path.read_text())
                    if value is None:
                        checker["native_metrics"].pop(field)
                    else:
                        checker["native_metrics"][field] = value
                    write_json(checker_path, checker)
                    row = analyzer.parse_attempt(
                        root, attempt, 1, "serialized-full", "TPC-C full mix"
                    )
                    self.assertFalse(row["valid"])
                    self.assertIn(message, row["invalid_reason"])

    def test_capture_only_publication_and_server_log_evidence_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            attempt = self.create_attempt(
                root, 1, "serialized-capture-only", 500.0
            )
            drain_path = attempt / "workload-drain.json"
            drain = json.loads(drain_path.read_text())
            drain["adapter_metrics"]["publication_sequence"] = 99
            reasons: list[str] = []
            self.assertFalse(
                analyzer.validate_live_drain(
                    drain, reasons, "serialized-capture-only", "drain"
                )
            )
            self.assertTrue(any("publication lifecycle" in reason for reason in reasons))

            drain["adapter_metrics"]["publication_sequence"] = 100
            drain["adapter_metrics"]["fragments_started"] = 0
            drain["adapter_metrics"]["fragments_accepted"] = 0
            drain["adapter_metrics"]["fragments_discarded"] = 0
            reasons = []
            self.assertTrue(
                analyzer.validate_live_drain(
                    drain, reasons, "serialized-capture-only", "post-prepare drain"
                )
            )
            self.assertFalse(reasons)

            server_path = attempt / "server-log-validation.json"
            server = json.loads(server_path.read_text())
            server["passed"] = False
            server["failure_matches"] = [
                {
                    "path": "main-server.txt",
                    "line": 7,
                    "category": "checker_close_failure",
                    "text": "online transaction checker did not close cleanly",
                }
            ]
            write_json(server_path, server)
            row = analyzer.parse_attempt(
                root, attempt, 1, "serialized-capture-only", "TPC-C full mix"
            )
            self.assertFalse(row["valid"])
            self.assertIn("server logs contain fatal/checker failure markers", row["invalid_reason"])

    def test_runner_static_full_mix_contract(self) -> None:
        runner = RUNNER.read_text()
        dsn_line = next(
            line for line in runner.splitlines() if line.startswith("readonly DSN_PARAMS=")
        )
        dsn = dsn_line.split('"', 2)[1]
        params = dict(urllib.parse.parse_qsl(dsn, strict_parsing=True))
        self.assertEqual(params["tidb_txn_mode"], "'optimistic'")
        self.assertEqual(
            params["tidb_index_lookup_pushdown_policy"], "'force'"
        )
        for required in (
            'TRANSACTION_WEIGHTS="45,43,4,4,4"',
            "readonly TPCC_PARTITION_TYPE=1",
            "readonly TPCC_PARTS=1",
            "readonly TPCC_USE_FK=false",
            "WORKLOAD_SEED=14",
            "--ddl-only",
            "--skip-ddl",
            "--retry-count 0",
            "--manifest",
            "tidb_sql_test.go",
            "source_state_sha256",
            "untracked_selection",
            "excluded_untracked_paths",
            "-action tpcc-digest",
            "-action tpcc-gate",
            "capacity-mb = 0",
            "tidb_store_batch_size=0",
            "tidb_allow_batch_cop=0",
            "tidb_txn_mode='optimistic'",
            "tidb_index_lookup_pushdown_policy='force'",
            "ATTEMPT_DIR_PATH=$4",
            "attempt_dir=$ATTEMPT_DIR_PATH",
            "prepare-reference-validation.json",
            "preparation-drain.json",
            "warmup-drain.json",
            "workload-drain.json",
        ):
            with self.subTest(required=required):
                self.assertIn(required, runner)
        self.assertIn("--baseline-full-only", runner)
        self.assertIn("--transaction-weights", runner)
        self.assertIn("SMOKE_WAREHOUSES", runner)
        self.assertIn('["baseline-original", "serialized-full"]', runner)
        self.assertIn('"modes": selected_modes', runner)
        self.assertIn(
            'refresh_analysis "$suite_root" "$trial_name"', runner
        )
        self.assertIn("tpcc-performance.csv", runner)
        self.assertEqual(runner.count("--ignore-error=false"), 7)
        self.assertEqual(runner.count("--silence=false"), 5)
        self.assertEqual(runner.count("--ignore-error=true"), 0)
        self.assertEqual(runner.count("--silence=true"), 2)
        self.assertEqual(runner.count('--partition-type "$TPCC_PARTITION_TYPE"'), 5)
        self.assertEqual(runner.count("--continue-on-write-conflict=true"), 2)
        self.assertEqual(runner.count('--parts "$TPCC_PARTS"'), 5)
        self.assertEqual(runner.count('--use-fk="$TPCC_USE_FK"'), 2)
        self.assertNotIn("--check-all", runner)
        self.assertIn("server-log-validation.json", runner)
        self.assertNotIn("SNAPSHOT_STORE", runner)
        mode_positions = [runner.index(f'"{mode}"') for mode in analyzer.MODES]
        self.assertEqual(mode_positions, sorted(mode_positions))

    def test_standard_post_workload_validator(self) -> None:
        validator = extract_python_heredoc("validate_post_check() {")
        condition_ids = [7, 3, 10, 1, 12, 5, 8, 2, 9, 4, 6]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            input_path = root / "post-check.txt"
            output_path = root / "post-check-validation.json"
            input_path.write_text(
                "\n".join(
                    [
                        *[
                            f"begin to check warehouse 1 at condition 3.3.2.{condition}"
                            for condition in condition_ids
                        ],
                        "Finished",
                    ]
                )
                + "\n"
            )
            result = run(
                ["python3", "-", "1", "0", str(input_path), str(output_path)],
                input=validator,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            validation = json.loads(output_path.read_text())
            self.assertTrue(validation["passed"])
            self.assertEqual(validation["condition_ids"], [*range(1, 11), 12])
            self.assertEqual(validation["conditions_per_warehouse"], 11)

            input_path.write_text(
                "begin to check warehouse 1 at condition 3.3.2.1\n"
                "execute check failed, err expected failure\n"
                "Finished\n"
            )
            result = run(
                ["python3", "-", "1", "0", str(input_path), str(output_path)],
                input=validator,
            )
            self.assertNotEqual(result.returncode, 0)
            validation = json.loads(output_path.read_text())
            self.assertFalse(validation["passed"])
            self.assertIn("execute check failed", validation["failure_markers"])
            self.assertIn("coverage mismatch", validation["reason"])

    def test_runner_rejects_invalid_arguments_before_creating_results(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = run(
                ["bash", str(RUNNER), "--result-root", "relative-results"]
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("--result-root must be absolute", result.stderr)

            source_result = run(
                [
                    "bash",
                    str(RUNNER),
                    "--result-root",
                    str(RUNNER.parents[1]),
                ]
            )
            self.assertNotEqual(source_result.returncode, 0)
            self.assertIn("must be outside source", source_result.stderr)

            prepare_result_root = root / "prepare-thread-failure"
            prepare_result = run(
                [
                    "bash",
                    str(RUNNER),
                    "--result-root",
                    str(prepare_result_root),
                    "--go-tpc",
                    "/bin/true",
                    "--prepare-threads",
                    "2",
                ]
            )
            self.assertNotEqual(prepare_result.returncode, 0)
            self.assertIn(
                "deterministic preparation requires --prepare-threads 1",
                prepare_result.stderr,
            )
            self.assertFalse(prepare_result_root.exists())

            custom_result = run(
                [
                    "bash",
                    str(RUNNER),
                    "--result-root",
                    str(root / "non-smoke-custom-weights"),
                    "--go-tpc",
                    "/bin/true",
                    "--transaction-weights",
                    "100,0,0,0,0",
                ]
            )
            self.assertNotEqual(custom_result.returncode, 0)
            self.assertIn(
                "non-default --transaction-weights require --smoke",
                custom_result.stderr,
            )

            no_new_order_result = run(
                [
                    "bash",
                    str(RUNNER),
                    "--smoke",
                    "--result-root",
                    str(root / "zero-new-order-weight"),
                    "--go-tpc",
                    "/bin/true",
                    "--transaction-weights",
                    "0,100,0,0,0",
                ]
            )
            self.assertNotEqual(no_new_order_result.returncode, 0)
            self.assertIn(
                "NewOrder transaction weight must be positive",
                no_new_order_result.stderr,
            )

    def test_runner_clears_duplicate_diagnostics_from_unchecked_servers(self) -> None:
        definitions = extract_shell_functions(
            "clear_online_checker_environment", "monotonic_ns"
        )
        result = run(
            ["bash", "-Eeuo", "pipefail", "-c", definitions + r"""
            export TIDB_ONLINE_CHECKER_DUPLICATE_READ_DIAGNOSTICS=1
            clear_online_checker_environment
            [[ -z ${TIDB_ONLINE_CHECKER_DUPLICATE_READ_DIAGNOSTICS+x} ]]
            """],
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_smoke_and_regular_manifest_setup_and_resume_execute(self) -> None:
        definitions = (
            extract_shell_functions("die", "require_value")
            + "\n"
            + extract_shell_functions("write_or_verify_experiment", "record_phase")
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifact_index = root / "build-artifacts.json"
            write_json(
                artifact_index,
                {
                    "build_type": "Release",
                    "tidb_build_command": "make server",
                    "inputs": {
                        "pristine_tidb": {"commit": "pristine"},
                        "instrumented_tidb": {"commit": "instrumented"},
                    },
                    "artifacts": {
                        "native_library": {"sha256": "1" * 64},
                        "pristine_tidb_server": {"sha256": "2" * 64},
                        "instrumented_tidb_server": {"sha256": "3" * 64},
                        "go_tpc": {"sha256": "4" * 64},
                        "go_tpc_patch": {"sha256": "5" * 64},
                    },
                },
            )
            shell = definitions + textwrap.dedent(
                r"""
                TIDB_CPUSET=0
                MODE_CSV=baseline-original,serialized-capture-only,serialized-order-only,serialized-full
                CLIENT_CPUSET=1
                SQL_HOST=127.0.0.1
                SQL_PORT=4400
                STATUS_PORT=14080
                DSN_PARAMS="tidb_index_lookup_pushdown_policy='force'"
                MONITOR_INTERVAL_SECONDS=1
                CHECKER_DRAIN_TIMEOUT=30m
                QUEUE_CAPACITY_BYTES=1024
                CAPTURE_CAPACITY_BYTES=2048
                RETAINED_STATE_CAPACITY_BYTES=4096
                QUEUE_TIMEOUT=5m
                CAPTURE_TIMEOUT=5m
                CHUNK_SEGMENT_BYTES=4096
                MAX_CACHED_CHUNK_SEGMENTS=256
                DUPLICATE_READ_DIAGNOSTICS=0
                TRANSACTION_WEIGHTS=45,43,4,4,4
                PREPARE_SEED=20260804
                WORKLOAD_SEED=14
                PREPARE_LOAD_TIME='2024-01-01 00:00:00'
                ARTIFACT_INDEX=$1
                TEST_ROOT=$2

                TRIALS=5
                WARMUP_TIME=60s
                RUN_TIME=600s
                WAREHOUSES=8
                THREADS=16
                PREPARE_THREADS=1
                OUTPUT_INTERVAL=10s
                STABILIZE_SECONDS=30
                SMOKE_WARMUP_TIME=5s
                SMOKE_RUN_TIME=10s
                SMOKE_WAREHOUSES=1
                SMOKE_THREADS=2
                SMOKE_PREPARE_THREADS=1
                SMOKE_OUTPUT_INTERVAL=1s
                SMOKE_STABILIZE_SECONDS=1

                RESUME=false
                RESULT_ROOT=$TEST_ROOT/explicit
                write_smoke_experiment "$RESULT_ROOT"
                RESULT_ROOT=$TEST_ROOT/regular
                write_requested_experiment "$RESULT_ROOT"
                write_smoke_experiment "$RESULT_ROOT/smoke"

                RESUME=true
                RESULT_ROOT=$TEST_ROOT/explicit
                write_smoke_experiment "$RESULT_ROOT"
                RESULT_ROOT=$TEST_ROOT/regular
                write_requested_experiment "$RESULT_ROOT"
                write_smoke_experiment "$RESULT_ROOT/smoke"

                RESUME=no
                MODE_CSV=baseline-original,serialized-full
                RESULT_ROOT=$TEST_ROOT/baseline-full
                write_requested_experiment "$RESULT_ROOT"
                write_smoke_experiment "$RESULT_ROOT/smoke"

                TRANSACTION_WEIGHTS=100,0,0,0,0
                SMOKE_WAREHOUSES=8
                DUPLICATE_READ_DIAGNOSTICS=1
                RESULT_ROOT=$TEST_ROOT/new-order-diagnostic
                write_smoke_experiment "$RESULT_ROOT"
                TRANSACTION_WEIGHTS=45,43,4,4,4
                SMOKE_WAREHOUSES=1
                DUPLICATE_READ_DIAGNOSTICS=0
                """
            )
            result = run(
                [
                    "bash", "-Eeuo", "pipefail", "-c", shell, "runner-test",
                    str(artifact_index), str(root),
                ]
            )
            self.assertEqual(result.returncode, 0, result.stderr)

            explicit = json.loads((root / "explicit" / "experiment.json").read_text())
            regular = json.loads((root / "regular" / "experiment.json").read_text())
            automatic_smoke = json.loads(
                (root / "regular" / "smoke" / "experiment.json").read_text()
            )
            baseline_full = json.loads(
                (root / "baseline-full" / "experiment.json").read_text()
            )
            baseline_full_smoke = json.loads(
                (root / "baseline-full" / "smoke" / "experiment.json").read_text()
            )
            new_order_diagnostic = json.loads(
                (root / "new-order-diagnostic" / "experiment.json").read_text()
            )
            self.assertTrue(explicit["smoke"])
            self.assertEqual(explicit["trials"], 1)
            self.assertEqual(explicit["workload"]["warehouses"], 1)
            self.assertFalse(regular["smoke"])
            self.assertEqual(regular["trials"], 5)
            self.assertEqual(regular["workload"]["warehouses"], 8)
            self.assertTrue(automatic_smoke["smoke"])
            self.assertEqual(automatic_smoke["trials"], 1)
            self.assertEqual(
                baseline_full["modes"], list(analyzer.BASELINE_FULL_MODES)
            )
            self.assertEqual(baseline_full["mode_order"], baseline_full["modes"])
            self.assertEqual(baseline_full_smoke["modes"], baseline_full["modes"])
            self.assertEqual(automatic_smoke["workload"]["warehouses"], 1)
            self.assertTrue(new_order_diagnostic["smoke"])
            self.assertEqual(
                new_order_diagnostic["transaction_weights"],
                [100, 0, 0, 0, 0],
            )
            self.assertTrue(
                new_order_diagnostic["checker_configuration"][
                    "duplicate_read_diagnostics"
                ]
            )
            self.assertEqual(new_order_diagnostic["workload"]["warehouses"], 8)

            mismatch = shell.rsplit("RESUME=false", 1)[0] + textwrap.dedent(
                r"""
                RESUME=true
                RESULT_ROOT=$TEST_ROOT/regular
                TRIALS=6
                write_requested_experiment "$RESULT_ROOT"
                """
            )
            mismatch_result = run(
                [
                    "bash", "-Eeuo", "pipefail", "-c", mismatch, "runner-test",
                    str(artifact_index), str(root),
                ]
            )
            self.assertNotEqual(mismatch_result.returncode, 0)
            self.assertIn(
                "resume manifest does not exactly match", mismatch_result.stderr
            )

    def test_source_snapshot_and_resume_verifier_execute(self) -> None:
        capture_script = extract_python_heredoc(
            'python3 - "$repository" "$name" "$patch_path"'
        )
        verify_script = extract_python_heredoc("verify_existing_artifacts() {")

        def initialize_repository(path: Path, checker: bool) -> None:
            tracked = path / ("online/tracked.cc" if checker else "tracked.go")
            tracked.parent.mkdir(parents=True)
            tracked.write_text("original\n")
            self.assertEqual(run(["git", "init", "-q", str(path)]).returncode, 0)
            self.assertEqual(
                run(["git", "-C", str(path), "add", "."]).returncode, 0
            )
            commit = run(
                [
                    "git",
                    "-C",
                    str(path),
                    "-c",
                    "user.name=runner-test",
                    "-c",
                    "user.email=runner-test@example.invalid",
                    "commit",
                    "-q",
                    "-m",
                    "base",
                ]
            )
            self.assertEqual(commit.returncode, 0, commit.stderr)
            tracked.write_text("modified\n")
            if checker:
                (path / "online/untracked.input").write_text("build input\n")
                (path / "doc").mkdir()
                (path / "doc/living-plan.md").write_text("excluded plan\n")
            else:
                (path / "untracked-build.go").write_text("package build\n")

        def capture(
            repository: Path, name: str, output: Path
        ) -> dict[str, object]:
            patch_path = output / f"{name}-tracked.patch"
            snapshot_path = output / f"{name}-untracked"
            state_path = output / f"{name}-source-state.json"
            patch = subprocess.check_output(
                [
                    "git",
                    "-C",
                    str(repository),
                    "diff",
                    "--binary",
                    "--no-ext-diff",
                    "HEAD",
                ]
            )
            patch_path.write_bytes(patch)
            result = run(
                [
                    "python3",
                    "-",
                    str(repository),
                    name,
                    str(patch_path),
                    str(snapshot_path),
                    str(state_path),
                ],
                input=capture_script,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(state_path.read_text())

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            checker = root / "checker"
            tidb = root / "tidb"
            output = root / "state"
            output.mkdir()
            initialize_repository(checker, True)
            initialize_repository(tidb, False)
            checker_state = capture(checker, "checker", output)
            tidb_state = capture(tidb, "instrumented-tidb", output)
            self.assertEqual(checker_state["untracked_selection"], "online/**")
            self.assertEqual(tidb_state["untracked_selection"], "all-untracked")
            self.assertIn(
                "doc/living-plan.md", checker_state["excluded_untracked_paths"]
            )

            inputs = {}
            for name in ("runner", "analyzer", "go-tpc", "go-tpc-patch"):
                path = root / name
                path.write_text(name + "\n")
                inputs[name] = path
            artifact = root / "artifact"
            artifact.write_text("artifact\n")
            index_path = root / "build-artifacts.json"
            write_json(
                index_path,
                {
                    "inputs": {
                        "checker": checker_state,
                        "instrumented_tidb": tidb_state,
                        "runner": {"sha256": sha256_file(inputs["runner"])},
                        "analyzer": {"sha256": sha256_file(inputs["analyzer"])},
                        "go_tpc_original": {
                            "sha256": sha256_file(inputs["go-tpc"])
                        },
                        "go_tpc_patch": {
                            "sha256": sha256_file(inputs["go-tpc-patch"])
                        },
                    },
                    "artifacts": {
                        "test": {
                            "path": str(artifact),
                            "sha256": sha256_file(artifact),
                        }
                    },
                },
            )

            arguments = [
                "python3",
                "-",
                str(index_path),
                str(checker),
                str(tidb),
                str(inputs["runner"]),
                str(inputs["analyzer"]),
                str(inputs["go-tpc"]),
                str(inputs["go-tpc-patch"]),
            ]
            verified = run(arguments, input=verify_script)
            self.assertEqual(verified.returncode, 0, verified.stderr)

            selected = checker / "online/untracked.input"
            selected.write_text("changed build input\n")
            changed = run(arguments, input=verify_script)
            self.assertNotEqual(changed.returncode, 0)
            self.assertIn("checker source changed", changed.stderr)
            selected.write_text("build input\n")

            excluded = checker / "doc/living-plan.md"
            excluded.write_text("changed but still excluded\n")
            excluded_result = run(arguments, input=verify_script)
            self.assertEqual(excluded_result.returncode, 0, excluded_result.stderr)

            untracked_files = checker_state["untracked_files"]
            snapshot_entry = next(
                entry for entry in untracked_files if entry["type"] == "file"
            )
            snapshot = Path(checker_state["untracked_snapshot_path"])
            snapshot_file = snapshot.joinpath(*Path(snapshot_entry["path"]).parts)
            snapshot_file.write_text("tampered snapshot\n")
            tampered = run(arguments, input=verify_script)
            self.assertNotEqual(tampered.returncode, 0)
            self.assertIn("untracked snapshot entry changed", tampered.stderr)

    def test_runner_and_analyzer_artifact_filename_contract(self) -> None:
        runner = RUNNER.read_text()
        for logical_name, filename in analyzer.ATTEMPT_ARTIFACTS.items():
            with self.subTest(logical_name=logical_name):
                self.assertIn(filename, runner)


    def test_missing_attempts_still_emit_the_fixed_mode_matrix(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_experiment(root, 1)
            result = analyzer.analyze(root)
            self.assertEqual(len(result["trial_rows"]), 4)
            self.assertEqual(result["invalid_run_count"], 4)
            for row in result["trial_rows"]:
                self.assertFalse(row["valid"])
                self.assertEqual(row["invalid_reason"], "no attempt directory found")


if __name__ == "__main__":
    unittest.main()
