#!/usr/bin/env python3

import json
import struct
import tempfile
import unittest
from pathlib import Path

import run_benchmark as bench


class HarnessTest(unittest.TestCase):
    def test_trace_header_and_event_order(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            larger = root / "larger.trace"
            smaller = root / "smaller.trace"
            larger.write_bytes(struct.pack("<8sIIQQ", b"COTRACE1", 1, 0, 2, 20))
            smaller.write_bytes(struct.pack("<8sIIQQ", b"COTRACE1", 1, 0, 9, 10))
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
            trace.write_bytes(struct.pack("<8sIIQQ", b"NOTTRACE", 1, 0, 1, 2))
            with self.assertRaisesRegex(ValueError, "invalid magic"):
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
            trace_path.write_bytes(struct.pack("<8sIIQQ", b"COTRACE1", 1, 0, 4, 20))
            trace = bench.read_trace_header(trace_path)

            co = self.make_process(root / "co", "snapshot-so-ww: fail (2.500 ms)\n", 1)
            co_row = bench.parse_checker_result("co-checker", trace, co)
            self.assertEqual((co_row["outcome"], co_row["verdict"]), ("success", "fail"))
            self.assertEqual(co_row["operations_per_second_checker"], 8000.0)

            timekiller = self.make_process(
                root / "tk",
                "Do NOT satisfy SI\n|  Checking time: 0.004000 s  |\n",
                0,
            )
            tk_row = bench.parse_checker_result("timekiller", trace, timekiller)
            self.assertEqual(
                (tk_row["outcome"], tk_row["verdict"], tk_row["checker_ms"]),
                ("success", "fail", 4.0),
            )

    def test_timekiller_requires_verdict_and_timing(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace_path = root / "input.trace"
            trace_path.write_bytes(struct.pack("<8sIIQQ", b"COTRACE1", 1, 0, 1, 1))
            trace = bench.read_trace_header(trace_path)
            process = self.make_process(root / "tk", "usage: TimeKiller\n", 0)
            row = bench.parse_checker_result("timekiller", trace, process)
            self.assertEqual(row["outcome"], "malformed_output")
            self.assertIn("missing TimeKiller", row["failure_evidence"])

    def test_conversion_stats_validate_header_and_file_size(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace_path = root / "input.trace"
            trace_path.write_bytes(struct.pack("<8sIIQQ", b"COTRACE1", 1, 0, 3, 8))
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
