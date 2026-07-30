import csv
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "tools" / "analyze_tpcc_global_lock.py"
RUNNER = Path(__file__).parents[1] / "scripts" / "run_tpcc_global_lock_comparison.sh"
SPEC = importlib.util.spec_from_file_location("analyze_tpcc_global_lock", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
analyzer = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = analyzer
SPEC.loader.exec_module(analyzer)


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n")


class TpccGlobalLockAnalyzerTest(unittest.TestCase):
    def create_experiment(self, root: Path, trials: int) -> None:
        write_json(
            root / "experiment.json",
            {
                "trials": trials,
                "modes": list(analyzer.MODES),
                "workload_label": "TPC-C NewOrder-only",
            },
        )

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
            {"mode": mode, "workload": "TPC-C NewOrder-only"},
        )
        write_json(
            attempt_dir / "runner-status.json",
            {"status": "complete", "go_tpc_pass": True},
        )
        write_json(attempt_dir / "completed.json", {"status": "complete"})
        for stage in ("prepare", "warmup", "workload"):
            write_json(attempt_dir / f"{stage}-validation.json", {"passed": True})
            write_json(attempt_dir / f"{stage}-status.json", {"exit_status": 0})

        successes = int(tpm_c * 100.0 / 60.0)
        workload_records = [
            {
                "Prefix": "[Summary]",
                "Operation": "NEW_ORDER",
                "Count": successes,
                "Takes(s)": 100.0,
                "TPM": tpm_c,
                "99th(ms)": 25.0 + trial,
            },
            {
                "Prefix": "[Summary]",
                "Operation": "NEW_ORDER_ERR",
                "Count": trial - 1,
            },
        ]
        (attempt_dir / "workload.txt").write_text(
            json.dumps(workload_records)
            + "\n"
            + json.dumps({"tpmC": tpm_c, "tpmTotal": tpm_c})
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
        client_exit = start + 100_000_000_000
        timing = {
            "workload_start_monotonic_ns": start,
            "workload_client_exit_monotonic_ns": client_exit,
            "server_shutdown_start_monotonic_ns": client_exit,
            "tidb_server_exit_monotonic_ns": client_exit + 6_000_000_000,
            # This huge post-check timestamp must not enter verdict timing.
            "tpcc_post_check_exit_monotonic_ns": client_exit + 10_000_000_000_000,
        }
        if mode != "baseline-original":
            timing["checker_result_available_monotonic_ns"] = (
                client_exit + 5_000_000_000
            )
        write_json(attempt_dir / "timing.json", timing)
        write_json(attempt_dir / "post-check-validation.json", {"passed": True})

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
            self.assertEqual(full["workload_start_to_verdict_seconds"], 105.0)
            self.assertEqual(full["post_workload_drain_to_verdict_seconds"], 5.0)
            self.assertIsNone(
                rows[(1, "baseline-original")]["workload_start_to_verdict_seconds"]
            )
            self.assertEqual(full["captured_minus_ordered_transaction_tail"], 4)
            self.assertEqual(
                full["accepted_publications_minus_ordered_transaction_tail"], 2
            )
            self.assertEqual(full["queue_average_residence_time_ns"], 100.0)
            # Backpressure is a reported metric, not an invalidity condition.
            self.assertTrue(full["valid"])

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
            self.assertEqual(
                summaries["serialized-order-only"]["valid_trial_count"], 1
            )
            self.assertEqual(
                summaries["serialized-order-only"]["invalid_trial_count"], 1
            )

            expected_outputs = {
                "tpcc-trials.csv",
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
            summary_json = json.loads((root / "tpcc-summary.json").read_text())
            self.assertFalse(summary_json["all_valid"])
            report = (root / "tpcc-report.md").read_text()
            self.assertIn("TPC-C NewOrder-only", report)
            self.assertIn("Invalid runs", report)
            self.assertIn(analyzer.CHECKER_METRIC_SCOPE, report)
            self.assertIn("Queue peak d/B", report)
            self.assertIn("Native close", report)
            report_lines = report.splitlines()
            for header in ("| Trial | Mode |", "| Mode | Valid/invalid |"):
                index = next(
                    i for i, line in enumerate(report_lines) if line.startswith(header)
                )
                self.assertEqual(
                    report_lines[index].count("|"),
                    report_lines[index + 1].count("|"),
                )

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
