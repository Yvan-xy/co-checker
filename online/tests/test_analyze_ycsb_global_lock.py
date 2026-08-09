import csv
import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "tools" / "analyze_ycsb_global_lock.py"
RUNNER = Path(__file__).parents[1] / "scripts" / "run_ycsb_global_lock_comparison.sh"
SPEC = importlib.util.spec_from_file_location("analyze_ycsb_global_lock", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
analyzer = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = analyzer
SPEC.loader.exec_module(analyzer)


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def experiment_identity(value: dict[str, object]) -> str:
    unsigned = dict(value)
    unsigned.pop("experiment_sha256", None)
    encoded = json.dumps(unsigned, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


class YCSBAnalyzerTest(unittest.TestCase):
    def create_experiment(
        self,
        root: Path,
        *,
        workloads: list[str] | None = None,
        trials: int = 1,
        smoke: bool = True,
    ) -> dict[str, object]:
        workloads = workloads or ["A"]
        workload_config = {
            workload: {"warmup_operations": 20, "measured_operations": 100}
            for workload in workloads
        }
        record = {
            "format_version": 1,
            "profile_id": "co-checker-ycsb-core-v1",
            "trials": trials,
            "workloads": workloads,
            "modes": list(analyzer.MODES),
            "mode_rotation": {
                str(trial): list(analyzer.MODES[trial - 1 :] + analyzer.MODES[: trial - 1])
                for trial in range(1, trials + 1)
            },
            "smoke": smoke,
            "diagnostic_only": False,
            "artifact_manifest_sha256": "a" * 64,
            "configuration": {
                "record_count": 1000,
                "field_count": 10,
                "field_length": 100,
                "threads": 4,
                "workloads": workload_config,
            },
        }
        record["experiment_sha256"] = experiment_identity(record)
        write_json(root / "experiment.json", record)
        return record

    @staticmethod
    def result(
        command: str,
        requested: int,
        measurements: dict[str, int],
    ) -> dict[str, object]:
        values = {
            name: {
                "count": count,
                "p50_us": 10,
                "p95_us": 20,
                "p99_us": 30,
                "p99_9_us": 40,
            }
            for name, count in measurements.items()
        }
        values["TOTAL"] = {
            "count": requested,
            "p50_us": 11,
            "p95_us": 21,
            "p99_us": 31,
            "p99_9_us": 41,
        }
        return {
            "format_version": analyzer.RESULT_FORMAT,
            "client_identity": "go-ycsb-v1.0.3-online-checker-v1",
            "command": command,
            "requested": requested,
            "attempted": requested,
            "successful": requested,
            "failed": 0,
            "error_classes": {},
            "measurements": values,
        }

    @staticmethod
    def checker(mode: str) -> dict[str, object]:
        pipeline = analyzer.PIPELINE_MODE[mode]
        adapter = {
            "attempts_registered": 110,
            "attempts_published": 100,
            "attempts_aborted": 10,
            "accepted_publications": 100,
            "publication_sequence": 100,
            "successful_write_commits": 40,
            "read_only_no_write_completions": 60,
            "active_attempts": 0,
            "active_finalizers": 0,
            "active_fragments": 0,
            "fragment_buffer_active_bytes": 0,
            "fragment_retained_active_bytes": 0,
            "fragments_started": 110,
            "fragments_accepted": 100,
            "fragments_discarded": 10,
            "fragments_late_discarded": 0,
            "fatal_calls": 0,
            "publish_errors": 0,
            "abort_errors": 0,
            "fragment_capture_errors": 0,
            "finalization_wait_nanos": 123,
        }
        native = {
            "closed": True,
            "fatal": False,
            "captured_transactions": 110,
            "captured_events": 550,
            "active_attempts": 0,
            "queue_reserved_bytes": 0,
            "queue_reserved_depth": 0,
            "queue_bytes": 0,
            "queue_depth": 0,
            "reorder_bytes": 0,
            "accepted_publications": 100,
            "accepted_events": 500,
            "recycled_transactions": 100 if pipeline == "capture-only" else 0,
            "ordered_transactions": 100 if pipeline != "capture-only" else 0,
            "checked_transactions": 100 if pipeline == "full" else 0,
            "checked_events": 500 if pipeline == "full" else 0,
            "violating_transactions": 0,
            "queue_backpressure_wait_count": 0,
            "queue_backpressure_ns": 0,
            "global_lock_wait_ns": 456,
            "global_lock_hold_ns": 789,
        }
        return {
            "format_version": 1,
            "mode": pipeline,
            "ordering_protocol": "serialized-global-lock",
            "outcome": analyzer.EXPECTED_OUTCOME[mode],
            "close_status": 0,
            "runtime_error": None,
            "close_error": None,
            "adapter_metrics": adapter,
            "native_metrics": native,
            "verdict": {"status": "pass"} if pipeline == "full" else None,
        }

    def create_attempt(
        self,
        root: Path,
        experiment: dict[str, object],
        workload: str,
        trial: int,
        mode: str,
        *,
        attempt: int = 1,
        duration: float = 10.0,
    ) -> Path:
        directory = (
            root
            / "trials"
            / f"workload-{workload}"
            / f"trial-{trial:03d}"
            / mode
            / f"attempt-{attempt:03d}"
        )
        directory.mkdir(parents=True)
        write_json(
            directory / "configuration.json",
            {
                "experiment_sha256": experiment["experiment_sha256"],
                "workload": workload,
                "trial": trial,
                "mode": mode,
            },
        )
        write_json(directory / "completed.json", {"status": "complete"})
        write_json(directory / "runner-status.json", {"status": "complete"})
        write_json(directory / "load-result.json", self.result("load", 1000, {"INSERT": 1000}))
        if workload in ("D", "E"):
            warmup_mix = {"INSERT": 2, "READ" if workload == "D" else "SCAN": 18}
            measured_mix = {"INSERT": 5, "READ" if workload == "D" else "SCAN": 95}
        elif workload == "F":
            warmup_mix = {"READ": 20, "UPDATE": 10, "READ_MODIFY_WRITE": 10}
            measured_mix = {"READ": 100, "UPDATE": 50, "READ_MODIFY_WRITE": 50}
        else:
            warmup_mix = {"READ": 10, "UPDATE": 10}
            measured_mix = {"READ": 50, "UPDATE": 50}
        warmup = self.result("run", 20, warmup_mix)
        measured = self.result("run", 100, measured_mix)
        write_json(directory / "warmup-result.json", warmup)
        write_json(directory / "workload-result.json", measured)
        top_mix, db_mix = analyzer.operation_mix(measured, workload)
        for filename in (
            "schema-validation.json",
            "load-validation.json",
            "gate-validation.json",
            "warmup-validation.json",
            "load-drain.json",
            "gate-drain.json",
            "warmup-drain.json",
            "post-check-validation.json",
            "server-log-validation.json",
        ):
            write_json(directory / filename, {"passed": True})
        write_json(
            directory / "workload-validation.json",
            {"passed": True, "actual_operation_mix": top_mix, "db_call_mix": db_mix},
        )
        client_exit = int(duration * 1_000_000_000)
        drain_ns = 0 if mode == analyzer.MODES[0] else 1_000_000_000
        write_json(
            directory / "timing.json",
            {
                "workload_start_monotonic_ns": 0,
                "workload_client_exit_monotonic_ns": client_exit,
                "checker_drain_complete_monotonic_ns": client_exit + drain_ns,
            },
        )
        write_json(
            directory / "workload-drain.json",
            {
                "passed": True,
                "cutoff_residual_transactions": 1 if drain_ns else 0,
                "cutoff_residual_events": 5 if mode == "serialized-full" else 0,
                "drained_transactions": 1 if drain_ns else 0,
                "drained_events": 5 if mode == "serialized-full" else 0,
            },
        )
        write_json(
            directory / "telemetry-validation.json",
            {
                "passed": True,
                "sample_count": 11,
                "sample_coverage_percent": 100.0,
                "max_sample_gap_seconds": 1.0,
                "cutoff_observation_delay_seconds": 0.05,
                "accepted_publications_delta": 100,
                "accepted_events_delta": 500,
                "ordered_transactions_delta": 100 if mode not in (analyzer.MODES[0], "serialized-capture-only") else 0,
                "checked_transactions_delta": 100 if mode == "serialized-full" else 0,
                "checked_events_delta": 500 if mode == "serialized-full" else 0,
                "transaction_service_rate_per_second": 100.0,
                "event_service_rate_per_second": 500.0,
                "peak_transaction_backlog": 3,
                "peak_event_backlog": 15,
                "peak_queue_depth": 3,
                "peak_queue_bytes": 1024,
                "capture_peak_bytes": 4096,
                "retained_state_peak_bytes": 8192,
                "tail_percent": 1.0,
                "sustained_backlog_growth": False,
            },
        )
        warmup_inserts = analyzer.metric_count(warmup, "INSERT")
        measured_inserts = analyzer.metric_count(measured, "INSERT")
        write_json(
            directory / "load-digest.json",
            {
                "total_rows": 1000,
                "schema_digest_sha256": "1" * 64,
                "data_digest_sha256": "2" * 64,
                "database_digest_sha256": "3" * 64,
            },
        )
        write_json(
            directory / "post-check-digest.json",
            {
                "total_rows": 1000 + warmup_inserts + measured_inserts,
                "schema_digest_sha256": "1" * 64,
                "data_digest_sha256": "4" * 64,
                "database_digest_sha256": "5" * 64,
            },
        )
        write_json(
            directory / "resource-summary.json",
            {
                "process_cpu_seconds": 7.5,
                "cpu_mean_percent": 75.0,
                "maximum_rss_kib": 100000,
                "maximum_hwm_kib": 110000,
            },
        )
        write_json(
            directory / "workload-resource.json",
            {"user_system_cpu_seconds": 3.5, "maximum_resident_set_size_kib": 20000},
        )
        if mode != analyzer.MODES[0]:
            write_json(
                directory / "workload-start.json",
                {
                    "scrape_success": True,
                    "adapter": {
                        "attempts_registered": 0.0,
                        "attempts_published": 0.0,
                        "attempts_aborted": 0.0,
                    },
                },
            )
            write_json(
                directory / "workload-cutoff.json",
                {
                    "scrape_success": True,
                    "adapter": {
                        "attempts_registered": 110.0,
                        "attempts_published": 100.0,
                        "attempts_aborted": 10.0,
                    },
                },
            )
            write_json(directory / "checker-result.json", self.checker(mode))
        return directory

    def test_valid_matrix_paired_overhead_and_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            experiment = self.create_experiment(root)
            durations = {
                "baseline-original": 10.0,
                "serialized-capture-only": 12.5,
                "serialized-order-only": 15.0,
                "serialized-full": 20.0,
            }
            for mode, duration in durations.items():
                self.create_attempt(root, experiment, "A", 1, mode, duration=duration)
            summary = analyzer.analyze(root)
            self.assertEqual(summary["invalid_rows"], 0)
            full = next(row for row in summary["rows"] if row["mode"] == "serialized-full")
            self.assertAlmostEqual(full["fixed_ops_per_second"], 5.0)
            self.assertAlmostEqual(full["completion_ops_per_second"], 100 / 21)
            self.assertAlmostEqual(full["paired_fixed_overhead_percent"], 50.0)
            self.assertEqual(full["measured_attempts_aborted"], 10.0)
            self.assertAlmostEqual(full["measured_attempt_abort_percent"], 1000 / 110)
            analyzer.write_outputs(root, summary)
            for filename in (
                "ycsb-trials.csv",
                "ycsb-performance.csv",
                "ycsb-summary.csv",
                "ycsb-summary.json",
                "ycsb-report.md",
            ):
                self.assertTrue((root / filename).is_file(), filename)
            with (root / "ycsb-trials.csv").open() as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 4)

    def test_missing_mode_and_newest_valid_append_only_attempt(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            experiment = self.create_experiment(root)
            first = self.create_attempt(root, experiment, "A", 1, "baseline-original", attempt=1)
            second = self.create_attempt(root, experiment, "A", 1, "baseline-original", attempt=2)
            write_json(second / "completed.json", {"status": "failed"})
            summary = analyzer.analyze(root)
            baseline = summary["rows"][0]
            self.assertTrue(baseline["valid"])
            self.assertEqual(baseline["attempt_path"], str(first))
            self.assertEqual(summary["invalid_rows"], 3)

    def test_manifest_mismatch_counter_imbalance_and_failed_verdict(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            experiment = self.create_experiment(root)
            for mode in analyzer.MODES:
                directory = self.create_attempt(root, experiment, "A", 1, mode)
                if mode == "serialized-capture-only":
                    configuration = json.loads((directory / "configuration.json").read_text())
                    configuration["experiment_sha256"] = "wrong"
                    write_json(directory / "configuration.json", configuration)
                elif mode == "serialized-order-only":
                    checker = json.loads((directory / "checker-result.json").read_text())
                    checker["adapter_metrics"]["attempts_aborted"] = 9
                    write_json(directory / "checker-result.json", checker)
                elif mode == "serialized-full":
                    checker = json.loads((directory / "checker-result.json").read_text())
                    checker["outcome"] = "violation"
                    checker["verdict"] = {"status": "violation"}
                    write_json(directory / "checker-result.json", checker)
            summary = analyzer.analyze(root)
            self.assertEqual(summary["invalid_rows"], 3)
            reasons = " ".join(row["invalid_reason"] for row in summary["rows"])
            self.assertIn("manifest mismatch", reasons)
            self.assertIn("attempt counters", reasons)
            self.assertIn("checker outcome", reasons)

    def test_workload_f_expansion_and_d_e_row_growth(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            experiment = self.create_experiment(root, workloads=["D", "E", "F"])
            for workload in ("D", "E", "F"):
                for mode in analyzer.MODES:
                    self.create_attempt(root, experiment, workload, 1, mode)
            summary = analyzer.analyze(root)
            self.assertEqual(summary["invalid_rows"], 0)
            for workload in ("D", "E"):
                row = next(row for row in summary["rows"] if row["workload"] == workload)
                self.assertEqual(row["warmup_insert_count"], 2)
                self.assertEqual(row["measured_insert_count"], 5)
                self.assertEqual(row["final_row_count"], 1007)
            full_f = next(
                row for row in summary["rows"]
                if row["workload"] == "F" and row["mode"] == "serialized-full"
            )
            self.assertEqual(json.loads(full_f["actual_operation_mix_json"]), {"READ": 50, "READ_MODIFY_WRITE": 50})
            self.assertEqual(json.loads(full_f["db_call_mix_json"]), {"READ": 100, "UPDATE": 50})

    def test_telemetry_gap_is_fail_closed_but_tail_is_only_warning(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            experiment = self.create_experiment(root)
            directories = {
                mode: self.create_attempt(root, experiment, "A", 1, mode)
                for mode in analyzer.MODES
            }
            bad = json.loads((directories["serialized-order-only"] / "telemetry-validation.json").read_text())
            bad.update({"passed": False, "max_sample_gap_seconds": 4.0})
            write_json(directories["serialized-order-only"] / "telemetry-validation.json", bad)
            warning = json.loads((directories["serialized-full"] / "telemetry-validation.json").read_text())
            warning.update({"tail_percent": 2.0, "sustained_backlog_growth": True})
            write_json(directories["serialized-full"] / "telemetry-validation.json", warning)
            summary = analyzer.analyze(root)
            order = next(row for row in summary["rows"] if row["mode"] == "serialized-order-only")
            full = next(row for row in summary["rows"] if row["mode"] == "serialized-full")
            self.assertFalse(order["valid"])
            self.assertTrue(full["valid"])
            self.assertIn("tail", full["sustainability_warning"])
            self.assertIn("backlog", full["sustainability_warning"])

    def test_atomic_output_replaces_existing_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "value.txt"
            path.write_text("old")
            analyzer.atomic_write(path, "new\n")
            self.assertEqual(path.read_text(), "new\n")
            self.assertEqual(list(path.parent.glob(".value.txt.*")), [])

    def test_noncanonical_mode_matrix_is_rejected(self) -> None:
        with self.assertRaises(analyzer.AnalysisError):
            analyzer.validate_experiment(
                {
                    "format_version": 1,
                    "workloads": ["A"],
                    "trials": 1,
                    "smoke": True,
                    "modes": ["baseline-original", "serialized-full"],
                    "mode_rotation": {"1": ["baseline-original", "serialized-full"]},
                }
            )

    def test_diagnostic_baseline_full_subset_is_accepted(self) -> None:
        modes = ["baseline-original", "serialized-full"]
        experiment = {
            "format_version": 1,
            "workloads": ["A"],
            "trials": 1,
            "smoke": False,
            "diagnostic_only": True,
            "modes": modes,
            "mode_rotation": {"1": modes},
        }
        self.assertEqual(analyzer.validate_experiment(experiment)["modes"], modes)

        experiment["modes"] = ["serialized-full"]
        experiment["mode_rotation"] = {"1": ["serialized-full"]}
        with self.assertRaises(analyzer.AnalysisError):
            analyzer.validate_experiment(experiment)

    def test_runner_static_contract_and_argument_failures(self) -> None:
        source = RUNNER.read_text()
        for token in (
            "baseline-original",
            "serialized-capture-only",
            "serialized-order-only",
            "serialized-full",
            "ycsb-schema",
            "assert-ycsb-empty",
            "ycsb-digest",
            "ycsb-gate",
            "mysql.create_table=false",
            "load.analyze=false",
            "mysql.strict_rows=true",
            "serialized-global-lock",
            "readonly TIDB_RETRY_LIMIT=100",
            "tidb_retry_limit=$TIDB_RETRY_LIMIT",
            "DIGEST_TIMEOUT=10m",
            '-timeout "$DIGEST_TIMEOUT"',
            "--modes",
            "mode_text.split",
            "FINAL_RESULTS_ONLY=true",
            "prune_successful_attempt_store",
            "level = \\\"$SERVER_LOG_LEVEL\\\"",
            "readonly LOAD_SEED=7732542672685676034",
            "readonly WARMUP_SEED=6300581044758642952",
            "readonly MEASURED_SEED=1251957853004150076",
            '"seed_policy": "fixed-v1"',
            "fresh-empty-store-and-seeded-ycsb-load",
        ):
            self.assertIn(token, source)
        command = subprocess.run(
            ["bash", str(RUNNER), "--smoke", "--result-root", "relative"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertNotEqual(command.returncode, 0)
        self.assertIn("absolute", command.stderr)


if __name__ == "__main__":
    unittest.main()
