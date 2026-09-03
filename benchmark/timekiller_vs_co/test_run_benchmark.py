#!/usr/bin/env python3

import csv
import json
import struct
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

import run_benchmark as bench


class HarnessTest(unittest.TestCase):
    def test_trace_header_and_event_order(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            larger = root / "larger.trace"
            smaller = root / "smaller.trace"
            larger.write_bytes(struct.pack("<8sIIQQ", b"COTRACE1", 1, 1, 2, 20))
            smaller.write_bytes(struct.pack("<8sIIQQ", b"COTRACE1", 1, 1, 9, 10))
            first = bench.read_trace_header(smaller)
            second = bench.read_trace_header(larger)
            self.assertEqual((first.transactions, first.events), (9, 10))
            self.assertEqual(
                [item.name for item in sorted([second, first], key=lambda item: item.events)],
                ["smaller.trace", "larger.trace"],
            )

    def test_rejects_bad_trace_header(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / "bad.trace"
            trace.write_bytes(struct.pack("<8sIIQQ", b"NOTTRACE", 1, 1, 1, 2))
            with self.assertRaisesRegex(ValueError, "invalid magic"):
                bench.read_trace_header(trace)

    def test_rejects_raw_cotrace_for_dense_benchmark(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / "raw.trace"
            trace.write_bytes(struct.pack("<8sIIQQ", b"COTRACE1", 1, 0, 1, 2))
            with self.assertRaisesRegex(ValueError, "not a dense-ID COTRACE1"):
                bench.read_trace_header(trace)

    def test_parses_gnu_time(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "time.txt"
            path.write_text(
                "\tElapsed (wall clock) time (h:mm:ss or m:ss): 1:02.50\n"
                "\tMaximum resident set size (kbytes): 123456\n"
                "Command terminated by signal 9\n",
                encoding="utf-8",
            )
            wall, rss, signal_number = bench.parse_gnu_time(path)
            self.assertEqual(wall, 62.5)
            self.assertEqual(rss, 123456)
            self.assertEqual(signal_number, 9)

    def test_checker_verdicts_are_separate_from_exit_status(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace_path = root / "input.trace"
            trace_path.write_bytes(struct.pack("<8sIIQQ", b"COTRACE1", 1, 1, 4, 20))
            trace = bench.read_trace_header(trace_path)

            co = self.make_process(root / "co", "snapshot-so-ww: fail (2.500 ms)\n", 1)
            co_row = bench.parse_checker_result("co-checker", trace, co)
            self.assertEqual((co_row["outcome"], co_row["verdict"]), ("success", "fail"))
            self.assertEqual(co_row["operations_per_second_checker"], 8000.0)
            self.assertEqual(co_row["peak_rss_mib"], 1.0)

            timekiller = self.make_process(
                root / "tk",
                "Do NOT satisfy SI\n"
                "|  Sorting time:            0.001000   s  |\n"
                "|  Checking time (excluding sorting): 0.004000 s  |\n",
                0,
            )
            tk_row = bench.parse_checker_result("timekiller", trace, timekiller)
            self.assertEqual(
                (
                    tk_row["outcome"],
                    tk_row["verdict"],
                    tk_row["checker_ms"],
                    tk_row["sorting_ms"],
                ),
                ("success", "fail", 4.0, 1.0),
            )

    def test_timekiller_rejects_old_or_incomplete_timing_contract(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace_path = root / "input.trace"
            trace_path.write_bytes(struct.pack("<8sIIQQ", b"COTRACE1", 1, 1, 1, 1))
            trace = bench.read_trace_header(trace_path)

            old_label = self.make_process(
                root / "old",
                "Satisfy SI\n"
                "|  Sorting time: 0.001000 s  |\n"
                "|  Checking time: 0.004000 s  |\n",
                0,
            )
            old_row = bench.parse_checker_result("timekiller", trace, old_label)
            self.assertEqual(old_row["outcome"], "malformed_output")
            self.assertIn("missing TimeKiller Checking time", old_row["failure_evidence"])

            missing_sort = self.make_process(
                root / "missing-sort",
                "Satisfy SI\n|  Checking time (excluding sorting): 0.004000 s  |\n",
                0,
            )
            missing_sort_row = bench.parse_checker_result(
                "timekiller", trace, missing_sort
            )
            self.assertEqual(missing_sort_row["outcome"], "malformed_output")
            self.assertIn("missing TimeKiller Sorting time", missing_sort_row["failure_evidence"])

    def test_timekiller_requires_verdict_and_timing(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace_path = root / "input.trace"
            trace_path.write_bytes(struct.pack("<8sIIQQ", b"COTRACE1", 1, 1, 1, 1))
            trace = bench.read_trace_header(trace_path)
            process = self.make_process(root / "tk", "usage: TimeKiller\n", 0)
            row = bench.parse_checker_result("timekiller", trace, process)
            self.assertEqual(row["outcome"], "malformed_output")
            self.assertIn("missing TimeKiller", row["failure_evidence"])

    def test_conversion_stats_validate_header_and_file_size(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace_path = root / "input.trace"
            trace_path.write_bytes(struct.pack("<8sIIQQ", b"COTRACE1", 1, 1, 3, 8))
            trace = bench.read_trace_header(trace_path)
            json_path = root / "output.json"
            json_path.write_text("[]\n", encoding="utf-8")
            stats_path = root / "stats.json"
            stats_path.write_text(
                json.dumps(
                    {
                        "transactions": 3,
                        "events": 8,
                        "keys": 2,
                        "values": 4,
                        "output_bytes": json_path.stat().st_size,
                    }
                ),
                encoding="utf-8",
            )
            process = self.make_process(root / "converter", "", 0)
            row, valid = bench.classify_conversion(trace, process, json_path, stats_path)
            self.assertTrue(valid)
            self.assertEqual(row["outcome"], "success")
            self.assertEqual(row["unique_values"], 4)
            self.assertEqual(row["peak_rss_mib"], 1.0)

    def test_summary_merges_tools_and_uses_arithmetic_average(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace_path = root / "input.trace"
            trace_path.write_bytes(struct.pack("<8sIIQQ", b"COTRACE1", 1, 1, 4, 20))
            trace = bench.read_trace_header(trace_path)

            rows = [
                self.trial_row(trace.name, "co-checker", "measure", "success", 1.0),
                self.trial_row(trace.name, "co-checker", "measure", "success", 2.0),
                self.trial_row(trace.name, "co-checker", "measure", "success", 100.0),
                self.trial_row(trace.name, "co-checker", "warmup", "success", 999.0),
                self.trial_row(trace.name, "timekiller", "measure", "success", 30.0, 3.0),
                self.trial_row(trace.name, "timekiller", "measure", "success", 60.0, 6.0),
                self.trial_row(trace.name, "timekiller", "measure", "timeout", None),
            ]
            with (root / "trials.csv").open("w", encoding="utf-8", newline="") as output:
                writer = csv.DictWriter(output, fieldnames=bench.TRIAL_FIELDS)
                writer.writeheader()
                writer.writerows(rows)

            summaries = bench.write_summary(
                SimpleNamespace(result_dir=root, run_id="test-run", trials=3), [trace]
            )
            self.assertEqual(len(summaries), 1)
            summary = summaries[0]
            self.assertAlmostEqual(summary["co_checker_ms"], 103.0 / 3.0)
            self.assertEqual(summary["timekiller_ms"], 45.0)
            self.assertEqual(summary["timekiller_sorting_ms"], 4.5)
            self.assertEqual(summary["co_checker_peak_rss_mib"], 1.0)
            self.assertEqual(summary["timekiller_peak_rss_mib"], 1.0)
            self.assertEqual(summary["co_checker_valid_trials"], 3)
            self.assertEqual(summary["timekiller_valid_trials"], 2)
            self.assertEqual(summary["timekiller_measured_attempts"], 3)
            self.assertEqual(summary["co_checker_verdict"], "pass")
            self.assertEqual(summary["timekiller_verdict"], "pass")
            self.assertEqual(summary["timekiller_outcomes"], '{"success":2,"timeout":1}')

            with (root / "summary.csv").open(encoding="utf-8", newline="") as source:
                written = list(csv.DictReader(source))
            self.assertEqual(len(written), 1)
            self.assertEqual(
                list(written[0]),
                [
                    "testcase",
                    "transactions",
                    "events",
                    "co_checker_ms",
                    "timekiller_ms",
                    "co_checker_peak_rss_mib",
                    "timekiller_peak_rss_mib",
                ],
            )
            self.assertEqual(written[0]["testcase"], trace.name)
            self.assertEqual(written[0]["co_checker_ms"], str(103.0 / 3.0))
            self.assertEqual(written[0]["timekiller_ms"], "")
            self.assertEqual(written[0]["timekiller_peak_rss_mib"], "")

    def test_default_run_is_exactly_ten_measured_trials(self) -> None:
        args = bench.make_parser().parse_args([])
        self.assertEqual(args.warmups, 0)
        self.assertEqual(args.trials, 10)
        self.assertFalse(args.smoke_test)

    @staticmethod
    def trial_row(
        trace: str,
        tool: str,
        phase: str,
        outcome: str,
        checker_ms: float | None,
        sorting_ms: float | None = None,
    ) -> dict[str, object]:
        return {
            "trace": trace,
            "tool": tool,
            "phase": phase,
            "outcome": outcome,
            "verdict": "pass" if outcome == "success" else "",
            "checker_ms": checker_ms,
            "sorting_ms": sorting_ms,
            "wall_seconds": checker_ms / 1000.0 if checker_ms is not None else None,
            "peak_rss_mib": 1.0,
            "transactions_per_second_checker": 1.0,
            "operations_per_second_checker": 2.0,
            "transactions_per_second_wall": 3.0,
            "operations_per_second_wall": 4.0,
        }

    @staticmethod
    def make_process(prefix: Path, stdout_text: str, exit_status: int) -> bench.ProcessResult:
        stdout = prefix.with_suffix(".stdout.log")
        stderr = prefix.with_suffix(".stderr.log")
        timing = prefix.with_suffix(".time.txt")
        command = prefix.with_suffix(".command.txt")
        stdout.write_text(stdout_text, encoding="utf-8")
        stderr.write_text("", encoding="utf-8")
        timing.write_text(
            "\tElapsed (wall clock) time (h:mm:ss or m:ss): 0:00.01\n"
            "\tMaximum resident set size (kbytes): 1024\n",
            encoding="utf-8",
        )
        command.write_text("fake\n", encoding="utf-8")
        return bench.ProcessResult(
            command=["fake"],
            start_utc="2026-01-01T00:00:00+00:00",
            exit_status=exit_status,
            signal_number=None,
            wall_seconds=0.01,
            peak_rss_kib=1024,
            timed_out=False,
            outer_watchdog_fired=False,
            stdout_path=stdout,
            stderr_path=stderr,
            time_path=timing,
            command_path=command,
        )


if __name__ == "__main__":
    unittest.main()
