#!/usr/bin/env python3
"""Reproducible TimeKiller/Chronos versus co-checker benchmark driver.

The driver deliberately uses only the Python standard library.  It validates
COTRACE1 headers, converts each trace exactly once, executes every process
under GNU time and GNU timeout, preserves per-process evidence, and writes CSV
rows incrementally so an interrupted multi-hour run remains inspectable.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import glob
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import signal
import statistics
import struct
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence


TRACE_HEADER = struct.Struct("<8sIIQQ")
TRACE_MAGIC = b"COTRACE1"
TRACE_VERSION = 1
TRACE_FLAGS = 1
PINNED_TIMEKILLER_COMMIT = "5b7a4a754cd2cf49d3a12b6a7b9f10a99cc5493f"
DEFAULT_JAVA_OPTIONS = "-Xms1g -Xmx48g -XX:+UseG1GC -XX:+ExitOnOutOfMemoryError"
DEFAULT_CONVERTER_TEMPLATE = "{converter} --stats {stats} {trace} {json}"

CO_RESULT_RE = re.compile(
    r"^snapshot-so-ww:\s+"
    r"(?P<verdict>pass|fail|inconclusive|unknown)\s+"
    r"\((?P<milliseconds>[0-9]+(?:\.[0-9]+)?)\s*ms\)$",
    re.MULTILINE,
)
TIMEKILLER_CHECK_RE = re.compile(
    r"^\|\s*Checking time \(excluding sorting\):\s*"
    r"(?P<seconds>[0-9]+(?:\.[0-9]+)?)\s+s\s*\|\s*$",
    re.MULTILINE,
)
TIMEKILLER_SORT_RE = re.compile(
    r"^\|\s*Sorting time:\s*(?P<seconds>[0-9]+(?:\.[0-9]+)?)\s+s\s*\|\s*$",
    re.MULTILINE,
)
TIMEKILLER_PASS_RE = re.compile(r"^Satisfy SI\s*$", re.MULTILINE)
TIMEKILLER_FAIL_RE = re.compile(r"^Do NOT satisfy SI\s*$", re.MULTILINE)


TRIAL_FIELDS = [
    "run_id",
    "trace",
    "trace_path",
    "transactions",
    "events",
    "tool",
    "phase",
    "trial",
    "command",
    "start_utc",
    "exit_status",
    "signal",
    "outcome",
    "verdict",
    "checker_ms",
    "sorting_ms",
    "wall_seconds",
    "peak_rss_mib",
    "transactions_per_second_checker",
    "operations_per_second_checker",
    "transactions_per_second_wall",
    "operations_per_second_wall",
    "timed_out",
    "oom",
    "failure_evidence",
    "stdout_log",
    "stderr_log",
    "time_log",
    "command_file",
]

CONVERSION_FIELDS = [
    "run_id",
    "trace",
    "trace_path",
    "input_bytes",
    "header_transactions",
    "header_events",
    "emitted_transactions",
    "emitted_events",
    "unique_keys",
    "unique_values",
    "reported_output_bytes",
    "actual_output_bytes",
    "command",
    "start_utc",
    "exit_status",
    "signal",
    "outcome",
    "wall_seconds",
    "peak_rss_mib",
    "timed_out",
    "oom",
    "failure_evidence",
    "json_path",
    "stats_path",
    "stdout_log",
    "stderr_log",
    "time_log",
    "command_file",
]

SUMMARY_FIELDS = [
    "testcase",
    "transactions",
    "events",
    "co_checker_ms",
    "timekiller_ms",
    "co_checker_peak_rss_mib",
    "timekiller_peak_rss_mib",
]


@dataclass(frozen=True)
class TraceInfo:
    path: Path
    size_bytes: int
    version: int
    flags: int
    transactions: int
    events: int

    @property
    def name(self) -> str:
        return self.path.name

    @property
    def slug(self) -> str:
        digest = hashlib.sha256(str(self.path).encode("utf-8")).hexdigest()[:10]
        stem = re.sub(r"[^A-Za-z0-9_.-]+", "_", self.path.stem)[:100]
        return f"{stem}-{digest}"


@dataclass
class ProcessResult:
    command: list[str]
    start_utc: str
    exit_status: int | None
    signal_number: int | None
    wall_seconds: float | None
    peak_rss_kib: int | None
    timed_out: bool
    outer_watchdog_fired: bool
    stdout_path: Path
    stderr_path: Path
    time_path: Path
    command_path: Path


class CsvRecorder:
    """Append and fsync one CSV record at a time."""

    def __init__(self, path: Path, fields: Sequence[str]) -> None:
        self.path = path
        self.fields = list(fields)
        self._file = path.open("w", encoding="utf-8", newline="")
        self._writer = csv.DictWriter(
            self._file, fieldnames=self.fields, extrasaction="raise"
        )
        self._writer.writeheader()
        self._sync()

    def append(self, row: dict[str, Any]) -> None:
        self._writer.writerow({field: csv_value(row.get(field)) for field in self.fields})
        self._sync()

    def _sync(self) -> None:
        self._file.flush()
        os.fsync(self._file.fileno())

    def close(self) -> None:
        self._file.close()

    def __enter__(self) -> "CsvRecorder":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


def csv_value(value: Any) -> Any:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    return value


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")


def read_trace_header(path: Path) -> TraceInfo:
    try:
        with path.open("rb") as source:
            raw = source.read(TRACE_HEADER.size)
    except OSError as error:
        raise ValueError(f"cannot read trace {path}: {error}") from error
    if len(raw) != TRACE_HEADER.size:
        raise ValueError(
            f"trace {path} has a truncated header: {len(raw)} of "
            f"{TRACE_HEADER.size} bytes"
        )
    magic, version, flags, transactions, events = TRACE_HEADER.unpack(raw)
    if magic != TRACE_MAGIC:
        raise ValueError(f"trace {path} has invalid magic {magic!r}")
    if version != TRACE_VERSION:
        raise ValueError(
            f"trace {path} uses unsupported COTRACE1 version {version}; "
            f"expected {TRACE_VERSION}"
        )
    if flags != TRACE_FLAGS:
        raise ValueError(
            f"trace {path} is not a dense-ID COTRACE1 input: flags=0x{flags:x}; "
            f"expected dense flags 0x{TRACE_FLAGS:x}"
        )
    return TraceInfo(
        path=path.resolve(),
        size_bytes=path.stat().st_size,
        version=version,
        flags=flags,
        transactions=transactions,
        events=events,
    )


def discover_traces(args: argparse.Namespace) -> list[TraceInfo]:
    candidates: list[Path] = []
    if args.trace:
        candidates.extend(Path(item).expanduser() for item in args.trace)
    else:
        patterns = args.trace_glob or [
            str(args.repo_root / "bin_traces/dense/tpcc/*.dense.trace")
        ]
        for pattern in patterns:
            expanded = Path(pattern).expanduser()
            if not expanded.is_absolute():
                expanded = args.repo_root / expanded
            candidates.extend(Path(item) for item in glob.glob(str(expanded)))

    unique: dict[Path, None] = {}
    for candidate in candidates:
        resolved = candidate.resolve()
        if resolved.is_file():
            unique[resolved] = None
        else:
            raise ValueError(f"trace does not exist or is not a file: {candidate}")
    if not unique:
        raise ValueError("trace selection matched no files")

    traces = [read_trace_header(path) for path in unique]
    traces.sort(key=lambda trace: (trace.events, trace.transactions, str(trace.path)))
    if args.max_traces is not None:
        traces = traces[: args.max_traces]
    return traces


def parse_elapsed_time(value: str) -> float:
    pieces = value.strip().split(":")
    if len(pieces) == 2:
        minutes, seconds = pieces
        return float(minutes) * 60.0 + float(seconds)
    if len(pieces) == 3:
        hours, minutes, seconds = pieces
        return float(hours) * 3600.0 + float(minutes) * 60.0 + float(seconds)
    raise ValueError(f"unrecognized GNU time elapsed value {value!r}")


def parse_gnu_time(path: Path) -> tuple[float | None, int | None, int | None]:
    wall_seconds: float | None = None
    peak_rss_kib: int | None = None
    signal_number: int | None = None
    if not path.exists():
        return wall_seconds, peak_rss_kib, signal_number
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()
        if stripped.startswith("Elapsed (wall clock) time"):
            try:
                wall_seconds = parse_elapsed_time(stripped.rsplit(": ", 1)[1])
            except (IndexError, ValueError):
                pass
        elif stripped.startswith("Maximum resident set size (kbytes):"):
            try:
                peak_rss_kib = int(stripped.rsplit(":", 1)[1].strip())
            except ValueError:
                pass
        elif stripped.startswith("Command terminated by signal"):
            try:
                signal_number = int(stripped.rsplit(" ", 1)[1])
            except ValueError:
                pass
    return wall_seconds, peak_rss_kib, signal_number


def command_environment() -> dict[str, str]:
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    environment["LANG"] = "C"
    # These can silently append or prepend Java arguments and would defeat a
    # fixed-options comparison.  The original values are captured in config.json.
    for name in ("JAVA_TOOL_OPTIONS", "JDK_JAVA_OPTIONS", "_JAVA_OPTIONS"):
        environment.pop(name, None)
    return environment


def run_timed_process(
    command: Sequence[str],
    *,
    evidence_prefix: Path,
    cwd: Path,
    timeout_seconds: float,
    time_binary: Path,
    timeout_binary: Path,
) -> ProcessResult:
    stdout_path = evidence_prefix.with_suffix(".stdout.log")
    stderr_path = evidence_prefix.with_suffix(".stderr.log")
    time_path = evidence_prefix.with_suffix(".time.txt")
    command_path = evidence_prefix.with_suffix(".command.txt")
    evidence_prefix.parent.mkdir(parents=True, exist_ok=True)

    timeout_text = f"{timeout_seconds:.6g}s"
    wrapped = [
        str(time_binary),
        "-v",
        "-o",
        str(time_path),
        str(timeout_binary),
        "--foreground",
        "--signal=TERM",
        "--kill-after=30s",
        timeout_text,
        *command,
    ]
    command_path.write_text(
        shlex.join(command) + "\n\nwrapped as:\n" + shlex.join(wrapped) + "\n",
        encoding="utf-8",
    )
    start_utc = utc_now()
    exit_status: int | None = None
    outer_watchdog_fired = False
    started = time.monotonic()
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        process = subprocess.Popen(
            wrapped,
            cwd=cwd,
            env=command_environment(),
            stdin=subprocess.DEVNULL,
            stdout=stdout,
            stderr=stderr,
            start_new_session=True,
        )
        try:
            # GNU timeout normally exits within kill-after.  This outer watchdog
            # prevents an unexpected wrapper failure from leaving a process tree.
            exit_status = process.wait(timeout=timeout_seconds + 45.0)
        except subprocess.TimeoutExpired:
            outer_watchdog_fired = True
            os.killpg(process.pid, signal.SIGKILL)
            exit_status = process.wait()

    wall_seconds, peak_rss_kib, signal_number = parse_gnu_time(time_path)
    if wall_seconds is None:
        wall_seconds = time.monotonic() - started
    timed_out = exit_status == 124 or outer_watchdog_fired
    return ProcessResult(
        command=list(command),
        start_utc=start_utc,
        exit_status=exit_status,
        signal_number=signal_number,
        wall_seconds=wall_seconds,
        peak_rss_kib=peak_rss_kib,
        timed_out=timed_out,
        outer_watchdog_fired=outer_watchdog_fired,
        stdout_path=stdout_path,
        stderr_path=stderr_path,
        time_path=time_path,
        command_path=command_path,
    )


def read_process_output(process: ProcessResult) -> str:
    output: list[str] = []
    for path in (process.stdout_path, process.stderr_path, process.time_path):
        if path.exists():
            output.append(path.read_text(encoding="utf-8", errors="replace"))
    return "\n".join(output)


def detect_oom(output: str, process: ProcessResult) -> tuple[bool, str]:
    patterns = [
        ("java.lang.OutOfMemoryError", "Java OutOfMemoryError"),
        ("Terminating due to java.lang.OutOfMemoryError", "Java ExitOnOutOfMemoryError"),
        ("Cannot allocate memory", "allocation failure"),
        ("std::bad_alloc", "C++ std::bad_alloc"),
        ("Killed process", "kernel OOM-kill message"),
    ]
    for needle, evidence in patterns:
        if needle.lower() in output.lower():
            return True, evidence
    if process.exit_status in (137, 9) and not process.timed_out:
        return False, "terminated with SIGKILL; possible external OOM kill"
    return False, ""


def process_failure_evidence(process: ProcessResult, output: str) -> tuple[bool, str]:
    oom, evidence = detect_oom(output, process)
    if process.outer_watchdog_fired:
        return oom, "outer watchdog killed the timed command"
    if process.timed_out:
        return oom, f"GNU timeout exceeded {process.wall_seconds:.3f} seconds"
    if evidence:
        return oom, evidence
    if process.signal_number is not None:
        return oom, f"command terminated by signal {process.signal_number}"
    return oom, ""


def require_nonnegative_count(stats: dict[str, Any], field: str) -> int:
    value = stats.get(field)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"stats field {field!r} must be a non-negative integer")
    return value


def render_converter_command(
    template: str,
    *,
    converter: Path,
    trace: Path,
    json_path: Path,
    stats_path: Path,
) -> list[str]:
    substitutions = {
        "converter": str(converter),
        "trace": str(trace),
        "json": str(json_path),
        "stats": str(stats_path),
    }
    try:
        tokens = shlex.split(template)
        command = [token.format(**substitutions) for token in tokens]
    except (KeyError, ValueError) as error:
        raise ValueError(f"invalid converter command template: {error}") from error
    if not command:
        raise ValueError("converter command template produced an empty command")
    return command


def classify_conversion(
    trace: TraceInfo,
    process: ProcessResult,
    json_path: Path,
    stats_path: Path,
) -> tuple[dict[str, Any], bool]:
    output = read_process_output(process)
    oom, evidence = process_failure_evidence(process, output)
    row: dict[str, Any] = {
        "input_bytes": trace.size_bytes,
        "header_transactions": trace.transactions,
        "header_events": trace.events,
        "exit_status": process.exit_status,
        "signal": process.signal_number,
        "wall_seconds": process.wall_seconds,
        "peak_rss_mib": kib_to_mib(process.peak_rss_kib),
        "timed_out": process.timed_out,
        "oom": oom,
        "failure_evidence": evidence,
        "json_path": json_path,
        "stats_path": stats_path,
        "stdout_log": process.stdout_path,
        "stderr_log": process.stderr_path,
        "time_log": process.time_path,
        "command_file": process.command_path,
    }
    if process.timed_out:
        row["outcome"] = "timeout"
        return row, False
    if oom:
        row["outcome"] = "oom"
        return row, False
    if process.signal_number is not None:
        row["outcome"] = "signal"
        if not row["failure_evidence"]:
            row["failure_evidence"] = (
                f"converter terminated by signal {process.signal_number}"
            )
        return row, False
    if process.exit_status != 0:
        row["outcome"] = "process_error"
        if not row["failure_evidence"]:
            row["failure_evidence"] = f"converter exited {process.exit_status}"
        return row, False

    try:
        if not stats_path.is_file():
            raise ValueError("converter did not create its stats JSON")
        stats = json.loads(stats_path.read_text(encoding="utf-8"))
        if not isinstance(stats, dict):
            raise ValueError("converter stats JSON must be an object")
        transactions = require_nonnegative_count(stats, "transactions")
        events = require_nonnegative_count(stats, "events")
        keys = require_nonnegative_count(stats, "keys")
        values = require_nonnegative_count(stats, "values")
        output_bytes = require_nonnegative_count(stats, "output_bytes")
        if transactions != trace.transactions:
            raise ValueError(
                f"emitted transaction count {transactions} differs from header "
                f"count {trace.transactions}"
            )
        if events != trace.events:
            raise ValueError(
                f"emitted event count {events} differs from header count {trace.events}"
            )
        if not json_path.is_file():
            raise ValueError("converter did not create its JSON output")
        actual_output_bytes = json_path.stat().st_size
        if output_bytes != actual_output_bytes:
            raise ValueError(
                f"reported output size {output_bytes} differs from actual size "
                f"{actual_output_bytes}"
            )
        row.update(
            {
                "emitted_transactions": transactions,
                "emitted_events": events,
                "unique_keys": keys,
                "unique_values": values,
                "reported_output_bytes": output_bytes,
                "actual_output_bytes": actual_output_bytes,
                "outcome": "success",
            }
        )
        return row, True
    except (OSError, json.JSONDecodeError, ValueError) as error:
        row["outcome"] = "invalid_output"
        row["failure_evidence"] = str(error)
        return row, False


def parse_checker_result(
    tool: str, trace: TraceInfo, process: ProcessResult
) -> dict[str, Any]:
    output = read_process_output(process)
    oom, evidence = process_failure_evidence(process, output)
    verdict = ""
    checker_ms: float | None = None
    sorting_ms: float | None = None
    malformed = ""

    if tool == "co-checker":
        matches = list(CO_RESULT_RE.finditer(output))
        if len(matches) == 1:
            verdict = matches[0].group("verdict")
            checker_ms = float(matches[0].group("milliseconds"))
        elif not matches:
            malformed = "missing snapshot-so-ww verdict/timing line"
        else:
            malformed = "multiple snapshot-so-ww verdict/timing lines"
        valid_exit = (
            (verdict == "pass" and process.exit_status == 0)
            or (verdict == "fail" and process.exit_status == 1)
            or (verdict in ("inconclusive", "unknown") and process.exit_status == 0)
        )
    elif tool == "timekiller":
        check_matches = list(TIMEKILLER_CHECK_RE.finditer(output))
        sort_matches = list(TIMEKILLER_SORT_RE.finditer(output))
        pass_matches = list(TIMEKILLER_PASS_RE.finditer(output))
        fail_matches = list(TIMEKILLER_FAIL_RE.finditer(output))
        if len(check_matches) == 1:
            checker_ms = float(check_matches[0].group("seconds")) * 1000.0
        elif not check_matches:
            malformed = "missing TimeKiller Checking time line"
        else:
            malformed = "multiple TimeKiller Checking time lines"
        if len(sort_matches) == 1:
            sorting_ms = float(sort_matches[0].group("seconds")) * 1000.0
        elif not sort_matches:
            malformed = join_evidence(malformed, "missing TimeKiller Sorting time line")
        else:
            malformed = join_evidence(malformed, "multiple TimeKiller Sorting time lines")
        if len(pass_matches) == 1 and not fail_matches:
            verdict = "pass"
        elif len(fail_matches) == 1 and not pass_matches:
            verdict = "fail"
        elif not pass_matches and not fail_matches:
            malformed = join_evidence(malformed, "missing TimeKiller SI verdict")
        else:
            malformed = join_evidence(malformed, "ambiguous TimeKiller SI verdict")
        valid_exit = process.exit_status == 0
    else:
        raise ValueError(f"unknown benchmark tool {tool!r}")

    if process.timed_out:
        outcome = "timeout"
    elif oom:
        outcome = "oom"
    elif process.signal_number is not None:
        outcome = "signal"
    elif not valid_exit:
        outcome = "checker_error"
        evidence = join_evidence(
            evidence, f"unexpected exit status {process.exit_status} for verdict {verdict!r}"
        )
    elif malformed:
        outcome = "malformed_output"
        evidence = join_evidence(evidence, malformed)
    else:
        outcome = "success"

    checker_seconds = checker_ms / 1000.0 if checker_ms and checker_ms > 0 else None
    wall_seconds = process.wall_seconds if process.wall_seconds and process.wall_seconds > 0 else None
    return {
        "exit_status": process.exit_status,
        "signal": process.signal_number,
        "outcome": outcome,
        "verdict": verdict,
        "checker_ms": checker_ms,
        "sorting_ms": sorting_ms,
        "wall_seconds": process.wall_seconds,
        "peak_rss_mib": kib_to_mib(process.peak_rss_kib),
        "transactions_per_second_checker": rate(trace.transactions, checker_seconds),
        "operations_per_second_checker": rate(trace.events, checker_seconds),
        "transactions_per_second_wall": rate(trace.transactions, wall_seconds),
        "operations_per_second_wall": rate(trace.events, wall_seconds),
        "timed_out": process.timed_out,
        "oom": oom,
        "failure_evidence": evidence,
        "stdout_log": process.stdout_path,
        "stderr_log": process.stderr_path,
        "time_log": process.time_path,
        "command_file": process.command_path,
    }


def join_evidence(left: str, right: str) -> str:
    if left and right:
        return f"{left}; {right}"
    return left or right


def rate(count: int, seconds: float | None) -> float | None:
    if seconds is None or seconds <= 0:
        return None
    return count / seconds


def kib_to_mib(value: int | float | None) -> float | None:
    if value is None:
        return None
    return value / 1024.0


def checker_command(args: argparse.Namespace, tool: str, trace_path: Path) -> list[str]:
    if tool == "co-checker":
        return [
            str(args.co_checker),
            "--format",
            "bin",
            "--checker",
            "snapshot-so-ww",
            *args.co_checker_arg,
            str(trace_path),
        ]
    if tool == "timekiller":
        return [
            str(args.java),
            *shlex.split(args.java_options),
            "-jar",
            str(args.timekiller_jar),
            "--mode",
            "fast",
            "--consistency_model",
            "SI",
            "--data_model",
            "kv",
            "--enable_session",
            "false",
            "--history_path",
            str(trace_path),
        ]
    raise ValueError(f"unknown benchmark tool {tool!r}")


def skipped_trial_row(
    args: argparse.Namespace,
    trace: TraceInfo,
    *,
    tool: str,
    phase: str,
    trial: int,
    reason: str,
    command: Sequence[str],
) -> dict[str, Any]:
    return {
        "run_id": args.run_id,
        "trace": trace.name,
        "trace_path": trace.path,
        "transactions": trace.transactions,
        "events": trace.events,
        "tool": tool,
        "phase": phase,
        "trial": trial,
        "command": shlex.join(command),
        "start_utc": utc_now(),
        "outcome": "skipped",
        "failure_evidence": reason,
    }


def run_checker_trial(
    args: argparse.Namespace,
    trace: TraceInfo,
    *,
    tool: str,
    phase: str,
    trial: int,
    input_path: Path,
) -> dict[str, Any]:
    command = checker_command(args, tool, input_path)
    prefix = args.result_dir / "logs" / trace.slug / phase / f"{tool}-{trial:02d}"
    process = run_timed_process(
        command,
        evidence_prefix=prefix,
        cwd=args.work_dir,
        timeout_seconds=args.timeout_seconds,
        time_binary=args.time_binary,
        timeout_binary=args.timeout_binary,
    )
    result = parse_checker_result(tool, trace, process)
    result.update(
        {
            "run_id": args.run_id,
            "trace": trace.name,
            "trace_path": trace.path,
            "transactions": trace.transactions,
            "events": trace.events,
            "tool": tool,
            "phase": phase,
            "trial": trial,
            "command": shlex.join(command),
            "start_utc": process.start_utc,
        }
    )
    return result


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open(encoding="utf-8", newline="") as source:
        return list(csv.DictReader(source))


def as_float(value: Any) -> float | None:
    try:
        if value is None or value == "":
            return None
        return float(value)
    except (TypeError, ValueError):
        return None


def average_field(rows: Sequence[dict[str, str]], field: str) -> float | None:
    values = [value for row in rows if (value := as_float(row.get(field))) is not None]
    return statistics.fmean(values) if values else None


def summary_prefix(tool: str) -> str:
    if tool == "co-checker":
        return "co_checker"
    if tool == "timekiller":
        return "timekiller"
    raise ValueError(f"unsupported summary tool: {tool}")


def summarize_tool(
    rows: Sequence[dict[str, str]], trace: TraceInfo, tool: str, prefix: str
) -> dict[str, Any]:
    measured = [
        row
        for row in rows
        if row["trace"] == trace.name
        and row["tool"] == tool
        and row["phase"] == "measure"
    ]
    valid = [row for row in measured if row["outcome"] == "success"]
    verdicts = sorted({row["verdict"] for row in valid if row["verdict"]})
    outcomes: dict[str, int] = {}
    for row in measured:
        outcomes[row["outcome"]] = outcomes.get(row["outcome"], 0) + 1

    summary = {
        f"{prefix}_measured_attempts": len(measured),
        f"{prefix}_valid_trials": len(valid),
        f"{prefix}_verdict": (
            verdicts[0] if len(verdicts) == 1 else ("mixed" if verdicts else "")
        ),
        f"{prefix}_ms": average_field(valid, "checker_ms"),
        f"{prefix}_wall_seconds": average_field(valid, "wall_seconds"),
        f"{prefix}_peak_rss_mib": average_field(valid, "peak_rss_mib"),
        f"{prefix}_transactions_per_second_checker": average_field(
            valid, "transactions_per_second_checker"
        ),
        f"{prefix}_operations_per_second_checker": average_field(
            valid, "operations_per_second_checker"
        ),
        f"{prefix}_transactions_per_second_wall": average_field(
            valid, "transactions_per_second_wall"
        ),
        f"{prefix}_operations_per_second_wall": average_field(
            valid, "operations_per_second_wall"
        ),
        f"{prefix}_outcomes": json.dumps(
            outcomes, sort_keys=True, separators=(",", ":")
        ),
    }
    if tool == "timekiller":
        summary["timekiller_sorting_ms"] = average_field(valid, "sorting_ms")
    return summary


def write_summary(args: argparse.Namespace, traces: Sequence[TraceInfo]) -> list[dict[str, Any]]:
    rows = read_csv(args.result_dir / "trials.csv")
    summaries: list[dict[str, Any]] = []
    for trace in traces:
        summary = {
            "run_id": args.run_id,
            "testcase": trace.name,
            "trace": trace.name,
            "transactions": trace.transactions,
            "events": trace.events,
        }
        summary.update(summarize_tool(rows, trace, "co-checker", "co_checker"))
        summary.update(summarize_tool(rows, trace, "timekiller", "timekiller"))
        summaries.append(summary)

    path = args.result_dir / "summary.csv"
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=SUMMARY_FIELDS)
        writer.writeheader()
        for row in summaries:
            csv_row = dict(row)
            for prefix in ("co_checker", "timekiller"):
                if (
                    row[f"{prefix}_measured_attempts"] != args.trials
                    or row[f"{prefix}_valid_trials"] != args.trials
                ):
                    csv_row[f"{prefix}_ms"] = None
                    csv_row[f"{prefix}_peak_rss_mib"] = None
            writer.writerow(
                {field: csv_value(csv_row.get(field)) for field in SUMMARY_FIELDS}
            )
    return summaries


def markdown_cell(value: Any) -> str:
    return str(value).replace("|", "\\|").replace("\n", " ")


def format_number(value: Any, digits: int = 3) -> str:
    numeric = as_float(value)
    return "—" if numeric is None else f"{numeric:,.{digits}f}"


def format_rate(value: Any) -> str:
    numeric = as_float(value)
    return "—" if numeric is None else f"{numeric:,.0f}"


def markdown_table(headers: Sequence[str], rows: Iterable[Sequence[Any]]) -> str:
    lines = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join("---" for _ in headers) + " |",
    ]
    for row in rows:
        lines.append("| " + " | ".join(markdown_cell(value) for value in row) + " |")
    return "\n".join(lines)


def write_report(
    args: argparse.Namespace,
    traces: Sequence[TraceInfo],
    summaries: Sequence[dict[str, Any]],
) -> None:
    conversions = read_csv(args.result_dir / "conversion.csv")
    trials = read_csv(args.result_dir / "trials.csv")
    summary_by_trace = {row["trace"]: row for row in summaries}
    conversion_by_trace = {row["trace"]: row for row in conversions}

    environment_text = (args.result_dir / "environment.txt").read_text(
        encoding="utf-8", errors="replace"
    )
    lines = [
        "# TimeKiller/Chronos versus snapshot-so-ww dense-input benchmark",
        "",
        f"Run ID: `{args.run_id}`. Generated at `{utc_now()}`.",
        "",
        "## Configuration and environment",
        "",
        f"- Warm-ups per tool and trace: {args.warmups}",
        f"- Measured trials per tool and trace: {args.trials}",
        f"- Per-process timeout: {args.timeout_seconds:g} seconds",
        f"- Java options: `{args.java_options}`",
        f"- Converted JSON retention: `{args.converted_retention}`",
        f"- TimeKiller expected commit: `{args.expected_timekiller_commit}`",
        f"- TimeKiller jar: `{args.timekiller_jar}`",
        f"- co-checker: `{args.co_checker}`",
        f"- converter: `{args.converter}`",
        "",
        "Commands used (paths vary per trace):",
        "",
        "```text",
        f"{args.co_checker} --format bin --checker snapshot-so-ww <trace>",
        f"{args.java} {args.java_options} -jar {args.timekiller_jar} --mode fast "
        "--consistency_model SI --data_model kv --enable_session false "
        "--history_path <converted.json>",
        f"converter template: {args.converter_command_template}",
        f"wrapper: {args.time_binary} -v -o <time-file> {args.timeout_binary} "
        "--foreground --signal=TERM --kill-after=30s <timeout> <command>",
        "```",
        "",
        "```text",
        environment_text.rstrip(),
        "```",
        "",
        "## Input traces",
        "",
        markdown_table(
            ["Trace", "Transactions", "Events", "Dense binary bytes"],
            (
                (trace.name, f"{trace.transactions:,}", f"{trace.events:,}", f"{trace.size_bytes:,}")
                for trace in traces
            ),
        ),
        "",
        "## One-time conversion",
        "",
        markdown_table(
            ["Trace", "Outcome", "Wall s", "Peak RSS MiB", "JSON GiB", "Keys", "Values"],
            (
                (
                    trace.name,
                    conversion_by_trace.get(trace.name, {}).get("outcome", "missing"),
                    format_number(conversion_by_trace.get(trace.name, {}).get("wall_seconds")),
                    format_number(
                        conversion_by_trace.get(trace.name, {}).get("peak_rss_mib")
                    ),
                    format_number(
                        divide(
                            conversion_by_trace.get(trace.name, {}).get("actual_output_bytes"),
                            1024.0**3,
                        )
                    ),
                    format_rate(conversion_by_trace.get(trace.name, {}).get("unique_keys")),
                    format_rate(conversion_by_trace.get(trace.name, {}).get("unique_values")),
                )
                for trace in traces
            ),
        ),
        "",
        "## Average checker-only and tool-run results",
        "",
        "Arithmetic averages include only successful measured trials. Each metric uses "
        "the trials for which that metric was recorded; the raw CSV preserves any missing "
        "measurements.",
        "",
        markdown_table(
            [
                "Trace",
                "Tool",
                "Valid/attempted",
                "Verdict",
                "Checker ms",
                "Sorting ms",
                "Tool wall s",
                "Peak RSS MiB",
                "Tx/s checker",
                "Ops/s checker",
                "Tx/s wall",
                "Ops/s wall",
            ],
            (
                (
                    trace.name,
                    tool,
                    f"{summary_by_trace[trace.name][summary_prefix(tool) + '_valid_trials']}/"
                    f"{summary_by_trace[trace.name][summary_prefix(tool) + '_measured_attempts']}",
                    summary_by_trace[trace.name][summary_prefix(tool) + "_verdict"] or "—",
                    format_number(
                        summary_by_trace[trace.name][summary_prefix(tool) + "_ms"]
                    ),
                    format_number(
                        summary_by_trace[trace.name].get(
                            summary_prefix(tool) + "_sorting_ms"
                        )
                    ),
                    format_number(
                        summary_by_trace[trace.name][
                            summary_prefix(tool) + "_wall_seconds"
                        ]
                    ),
                    format_number(
                        summary_by_trace[trace.name][
                            summary_prefix(tool) + "_peak_rss_mib"
                        ]
                    ),
                    format_rate(
                        summary_by_trace[trace.name][
                            summary_prefix(tool) + "_transactions_per_second_checker"
                        ]
                    ),
                    format_rate(
                        summary_by_trace[trace.name][
                            summary_prefix(tool) + "_operations_per_second_checker"
                        ]
                    ),
                    format_rate(
                        summary_by_trace[trace.name][
                            summary_prefix(tool) + "_transactions_per_second_wall"
                        ]
                    ),
                    format_rate(
                        summary_by_trace[trace.name][
                            summary_prefix(tool) + "_operations_per_second_wall"
                        ]
                    ),
                )
                for trace in traces
                for tool in ("co-checker", "timekiller")
            ),
        ),
        "",
        "## Failures, timeouts, and OOMs",
        "",
    ]

    failure_counts: dict[tuple[str, str, str, str], int] = {}
    for row in conversions:
        if row["outcome"] != "success":
            key = (
                row["trace"],
                "conversion",
                row["outcome"],
                row["failure_evidence"] or row["stderr_log"],
            )
            failure_counts[key] = failure_counts.get(key, 0) + 1
    for row in trials:
        if row["outcome"] != "success":
            key = (
                row["trace"],
                row["tool"],
                row["outcome"],
                row["failure_evidence"] or row["stderr_log"],
            )
            failure_counts[key] = failure_counts.get(key, 0) + 1
    if failure_counts:
        failures = [
            (trace, tool, count, outcome, evidence)
            for (trace, tool, outcome, evidence), count in sorted(
                failure_counts.items()
            )
        ]
        lines.extend(
            [
                markdown_table(
                    ["Trace", "Tool", "Count", "Outcome", "Evidence"], failures
                ),
                "",
            ]
        )
    else:
        lines.extend(["No process failures, timeouts, OOMs, or malformed outputs were recorded.", ""])

    lines.extend(
        [
            "## Measurement caveats",
            "",
            "- Raw-to-dense preprocessing is outside both measured tools. co-checker reads "
            "the persistent dense-ID COTRACE1 binary; TimeKiller reads JSON projected from "
            "those exact IDs without re-interning raw keys or values.",
            "- Both reported checker timers exclude compact-input decoding, construction of "
            "their neutral transaction/operation models, and commit-timestamp sorting. "
            "TimeKiller reports sorting separately; its self-identifying `Checking time "
            "(excluding sorting)` includes explicit pre-check GC, dense initial state and "
            "per-key writer-index construction, and the SI check. co-checker includes "
            "snapshot/change-list construction and checking. Both exclude result printing.",
            "- Tool-run wall time includes that tool's compact-input parsing/loading and "
            "output. The encodings still differ (dense binary versus JSON), so wall time is "
            "not a byte-for-byte same-input comparison. The one-time dense-to-JSON projection "
            "is reported separately. A large "
            "TimeKiller violation set can also increase wall time and log size after its "
            "reported checker timer stops.",
            "- `snapshot-so-ww` and Chronos's timestamp-based SI checker implement different "
            "decision procedures. Treat these results as a performance comparison on the "
            "same valid histories, not as evidence that the two predicates are equivalent. "
            "The harness records verdict differences but does not fail the run on them.",
            "- TimeKiller runs with `--enable_session false`; `snapshot-so-ww` still uses "
            "session order when constructing snapshots. This avoids folding TimeKiller's "
            "load-time SESSION pass into its wall measurement, but it is a semantic "
            "asymmetry that must be disclosed.",
            "- Each TimeKiller trial starts a new JVM. Warm-ups can warm filesystem/page "
            "cache state, but do not preserve JVM JIT state between trials.",
            "- Caches were not dropped. Within each phase, co-checker always ran before "
            "TimeKiller, so fixed order and page-cache effects may bias the comparison. "
            "Background host activity, CPU frequency scaling, and NUMA placement can also "
            "affect results.",
            "- Peak RSS is GNU time's whole-process maximum for compact-input loading, model "
            "construction, and checking, converted from native KiB to MiB. It is not "
            "checker-owned heap allocation. Fixed Java heap limits and the exact JVM are "
            "recorded above.",
            "",
            "Raw evidence is in `conversion.csv`, `trials.csv`, `summary.csv`, and `logs/` "
            "beside this report.",
            "",
        ]
    )
    (args.result_dir / "report.md").write_text("\n".join(lines), encoding="utf-8")


def divide(value: Any, divisor: float) -> float | None:
    numeric = as_float(value)
    if numeric is None:
        return None
    return numeric / divisor


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def short_command_output(command: Sequence[str], cwd: Path | None = None) -> str:
    try:
        completed = subprocess.run(
            command,
            cwd=cwd,
            env=command_environment(),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=20,
            check=False,
        )
        return completed.stdout.strip()
    except (OSError, subprocess.TimeoutExpired) as error:
        return f"unavailable: {error}"


def git_value(source: Path, *arguments: str) -> str:
    return short_command_output(["git", "-C", str(source), *arguments])


def capture_environment(args: argparse.Namespace) -> dict[str, Any]:
    cpu_model = ""
    mem_total = ""
    try:
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
            if line.startswith("model name"):
                cpu_model = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    try:
        for line in Path("/proc/meminfo").read_text(encoding="utf-8").splitlines():
            if line.startswith("MemTotal:"):
                mem_total = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass

    info: dict[str, Any] = {
        "captured_utc": utc_now(),
        "platform": platform.platform(),
        "uname": " ".join(platform.uname()),
        "cpu_count": os.cpu_count(),
        "cpu_model": cpu_model,
        "mem_total": mem_total,
        "python": sys.version.replace("\n", " "),
        "java_version": short_command_output([str(args.java), "-version"]),
        "gnu_time_version": short_command_output([str(args.time_binary), "--version"]),
        "timeout_version": short_command_output([str(args.timeout_binary), "--version"]),
        "repo_commit": git_value(args.repo_root, "rev-parse", "HEAD"),
        "repo_status": git_value(args.repo_root, "status", "--short"),
        "timekiller_commit": git_value(args.timekiller_source, "rev-parse", "HEAD"),
        "timekiller_status": git_value(args.timekiller_source, "status", "--short"),
        "co_checker_sha256": sha256_file(args.co_checker),
        "converter_sha256": sha256_file(args.converter),
        "timekiller_jar_sha256": sha256_file(args.timekiller_jar),
        "inherited_java_environment": {
            name: os.environ.get(name)
            for name in ("JAVA_TOOL_OPTIONS", "JDK_JAVA_OPTIONS", "_JAVA_OPTIONS")
        },
    }
    readable = [
        f"captured_utc: {info['captured_utc']}",
        f"platform: {info['platform']}",
        f"uname: {info['uname']}",
        f"cpu_count: {info['cpu_count']}",
        f"cpu_model: {info['cpu_model']}",
        f"mem_total: {info['mem_total']}",
        f"python: {info['python']}",
        f"repo_commit: {info['repo_commit']}",
        f"timekiller_commit: {info['timekiller_commit']}",
        f"co_checker_sha256: {info['co_checker_sha256']}",
        f"converter_sha256: {info['converter_sha256']}",
        f"timekiller_jar_sha256: {info['timekiller_jar_sha256']}",
        "java_version:",
        str(info["java_version"]),
        "gnu_time_version:",
        str(info["gnu_time_version"]),
        "timeout_version:",
        str(info["timeout_version"]),
    ]
    (args.result_dir / "environment.txt").write_text(
        "\n".join(readable) + "\n", encoding="utf-8"
    )
    return info


def write_trace_csv(result_dir: Path, traces: Sequence[TraceInfo]) -> None:
    fields = ["order", "trace", "trace_path", "transactions", "events", "binary_bytes", "version", "flags"]
    with (result_dir / "traces.csv").open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for order, trace in enumerate(traces, start=1):
            writer.writerow(
                {
                    "order": order,
                    "trace": trace.name,
                    "trace_path": trace.path,
                    "transactions": trace.transactions,
                    "events": trace.events,
                    "binary_bytes": trace.size_bytes,
                    "version": trace.version,
                    "flags": trace.flags,
                }
            )


def resolve_executable(value: str | Path, description: str) -> Path:
    text = str(value)
    if "/" in text:
        path = Path(text).expanduser().resolve()
    else:
        found = shutil.which(text)
        if found is None:
            raise ValueError(f"cannot find {description} executable {text!r}")
        path = Path(found).resolve()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise ValueError(f"{description} is not an executable file: {path}")
    return path


def prepare_paths(args: argparse.Namespace) -> None:
    args.repo_root = args.repo_root.expanduser().resolve()
    args.timekiller_source = args.timekiller_source.expanduser().resolve()
    args.co_checker = resolve_executable(args.co_checker, "co-checker")
    args.converter = resolve_executable(args.converter, "converter")
    args.java = resolve_executable(args.java, "Java")
    args.time_binary = resolve_executable(args.time_binary, "GNU time")
    args.timeout_binary = resolve_executable(args.timeout_binary, "GNU timeout")
    args.timekiller_jar = args.timekiller_jar.expanduser().resolve()
    if not args.timekiller_jar.is_file():
        raise ValueError(f"TimeKiller jar does not exist: {args.timekiller_jar}")
    if not args.timekiller_source.is_dir():
        raise ValueError(f"TimeKiller source does not exist: {args.timekiller_source}")
    actual_commit = git_value(args.timekiller_source, "rev-parse", "HEAD").strip()
    if actual_commit != args.expected_timekiller_commit and not args.allow_commit_mismatch:
        raise ValueError(
            f"TimeKiller source is at {actual_commit!r}, expected "
            f"{args.expected_timekiller_commit}; use --allow-commit-mismatch only for an "
            "intentional non-comparable run"
        )

    args.result_dir = args.result_dir.expanduser().resolve()
    args.work_dir = args.work_dir.expanduser().resolve()
    if args.result_dir == args.work_dir:
        raise ValueError("--result-dir and --work-dir must be different directories")
    if args.result_dir in args.work_dir.parents or args.work_dir in args.result_dir.parents:
        raise ValueError("--result-dir and --work-dir must not contain one another")
    for path, description in (
        (args.result_dir, "result directory"),
        (args.work_dir, "work directory"),
    ):
        if path.exists() and any(path.iterdir()):
            raise ValueError(f"{description} must be new or empty: {path}")
        path.mkdir(parents=True, exist_ok=True)
    (args.work_dir / "converted").mkdir(exist_ok=True)


def validate_args(args: argparse.Namespace) -> None:
    if args.warmups < 0:
        raise ValueError("--warmups must be non-negative")
    if args.trials < 1:
        raise ValueError("--trials must be at least one (the default is ten)")
    if args.timeout_seconds <= 0:
        raise ValueError("--timeout-seconds must be positive")
    if args.conversion_timeout_seconds is not None and args.conversion_timeout_seconds <= 0:
        raise ValueError("--conversion-timeout-seconds must be positive")
    if args.max_traces is not None and args.max_traces < 1:
        raise ValueError("--max-traces must be positive")
    try:
        shlex.split(args.java_options)
    except ValueError as error:
        raise ValueError(f"invalid --java-options: {error}") from error


def make_parser() -> argparse.ArgumentParser:
    script = Path(__file__).resolve()
    repo_root = script.parents[2]
    timekiller_root = repo_root.parent / "timekiller"
    run_id = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    external_root = Path("/tmp/co-checker-timekiller-benchmark")
    parser = argparse.ArgumentParser(
        description=(
            "Convert COTRACE1 traces once and benchmark snapshot-so-ww against "
            "TimeKiller/Chronos SI fast mode with GNU time evidence."
        ),
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--repo-root", type=Path, default=repo_root)
    parser.add_argument(
        "--trace",
        action="append",
        default=[],
        help="exact trace path; repeatable and overrides --trace-glob",
    )
    parser.add_argument(
        "--trace-glob",
        action="append",
        default=[],
        help="trace glob relative to repo root or absolute; repeatable",
    )
    parser.add_argument("--max-traces", type=int, help="limit the event-sorted trace list")
    parser.add_argument("--warmups", type=int, default=0)
    parser.add_argument("--trials", type=int, default=10)
    parser.add_argument("--timeout-seconds", type=float, default=7200.0)
    parser.add_argument(
        "--conversion-timeout-seconds",
        type=float,
        help="converter timeout; defaults to --timeout-seconds",
    )
    parser.add_argument("--java", default="java")
    parser.add_argument(
        "--java-options",
        default=DEFAULT_JAVA_OPTIONS,
        help="fixed JVM arguments, parsed with POSIX shell quoting but never run through a shell",
    )
    parser.add_argument("--timekiller-source", type=Path, default=timekiller_root)
    parser.add_argument(
        "--timekiller-jar",
        type=Path,
        default=timekiller_root / "target/TimeKiller-jar-with-dependencies.jar",
    )
    parser.add_argument(
        "--expected-timekiller-commit", default=PINNED_TIMEKILLER_COMMIT
    )
    parser.add_argument(
        "--allow-commit-mismatch",
        action="store_true",
        help="permit a TimeKiller source commit other than the pinned comparison commit",
    )
    parser.add_argument(
        "--co-checker",
        default=str(repo_root / "build-benchmark-release/co-checker"),
    )
    parser.add_argument(
        "--co-checker-arg",
        action="append",
        default=[],
        help="additional co-checker argument placed before the trace path; repeatable",
    )
    parser.add_argument(
        "--converter",
        default=str(repo_root / "build-benchmark-release/cotrace-to-timekiller"),
    )
    parser.add_argument(
        "--converter-command-template",
        default=DEFAULT_CONVERTER_TEMPLATE,
        help="token template using {converter}, {trace}, {json}, and {stats}",
    )
    parser.add_argument(
        "--converted-retention",
        choices=("all", "failed", "none"),
        default="none",
        help="retain all JSON, only traces with conversion/checker errors, or none",
    )
    parser.add_argument(
        "--smoke-test",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="run one unmeasured trial of both tools on the smallest trace first",
    )
    parser.add_argument("--time-binary", default="/usr/bin/time")
    parser.add_argument("--timeout-binary", default="/usr/bin/timeout")
    parser.add_argument("--run-id", default=run_id)
    parser.add_argument(
        "--result-dir", type=Path, default=external_root / "results" / run_id
    )
    parser.add_argument("--work-dir", type=Path, default=external_root / "work" / run_id)
    parser.add_argument(
        "--list-traces",
        action="store_true",
        help=(
            "validate and print selected trace headers, ordered by event count, "
            "without requiring built tools"
        ),
    )
    return parser


def print_traces(traces: Sequence[TraceInfo]) -> None:
    print("order\tevents\ttransactions\tbytes\ttrace")
    for order, trace in enumerate(traces, start=1):
        print(
            f"{order}\t{trace.events}\t{trace.transactions}\t"
            f"{trace.size_bytes}\t{trace.path}"
        )


def serialize_config(args: argparse.Namespace, environment: dict[str, Any]) -> dict[str, Any]:
    config: dict[str, Any] = {}
    for key, value in vars(args).items():
        if isinstance(value, Path):
            config[key] = str(value)
        else:
            config[key] = value
    config["environment"] = environment
    return config


def cleanup_converted(
    args: argparse.Namespace,
    json_path: Path,
    stats_path: Path,
    *,
    retain_for_failure: bool,
) -> None:
    keep = args.converted_retention == "all" or (
        args.converted_retention == "failed" and retain_for_failure
    )
    if keep:
        return
    for path in (json_path, stats_path):
        try:
            path.unlink()
        except FileNotFoundError:
            pass


def execute(args: argparse.Namespace, traces: Sequence[TraceInfo]) -> int:
    environment = capture_environment(args)
    config = serialize_config(args, environment)
    (args.result_dir / "config.json").write_text(
        json.dumps(config, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    write_trace_csv(args.result_dir, traces)

    any_run_failure = False
    interrupted = False
    conversion_path = args.result_dir / "conversion.csv"
    trial_path = args.result_dir / "trials.csv"
    with CsvRecorder(conversion_path, CONVERSION_FIELDS) as conversions, CsvRecorder(
        trial_path, TRIAL_FIELDS
    ) as trials:
        try:
            for trace_index, trace in enumerate(traces):
                print(
                    f"[{trace_index + 1}/{len(traces)}] {trace.name}: "
                    f"{trace.transactions:,} transactions, {trace.events:,} events",
                    flush=True,
                )
                json_path = args.work_dir / "converted" / f"{trace.slug}.json"
                stats_path = args.work_dir / "converted" / f"{trace.slug}.stats.json"
                converter_command = render_converter_command(
                    args.converter_command_template,
                    converter=args.converter,
                    trace=trace.path,
                    json_path=json_path,
                    stats_path=stats_path,
                )
                conversion_prefix = (
                    args.result_dir / "logs" / trace.slug / "conversion" / "converter-00"
                )
                conversion_process = run_timed_process(
                    converter_command,
                    evidence_prefix=conversion_prefix,
                    cwd=args.work_dir,
                    timeout_seconds=(
                        args.conversion_timeout_seconds or args.timeout_seconds
                    ),
                    time_binary=args.time_binary,
                    timeout_binary=args.timeout_binary,
                )
                conversion_row, conversion_ok = classify_conversion(
                    trace, conversion_process, json_path, stats_path
                )
                conversion_row.update(
                    {
                        "run_id": args.run_id,
                        "trace": trace.name,
                        "trace_path": trace.path,
                        "command": shlex.join(converter_command),
                        "start_utc": conversion_process.start_utc,
                    }
                )
                conversions.append(conversion_row)
                print(
                    f"  conversion: {conversion_row['outcome']} "
                    f"({format_number(conversion_row.get('wall_seconds'))} s)",
                    flush=True,
                )
                trace_failed = not conversion_ok
                any_run_failure |= not conversion_ok

                phases: list[tuple[str, int]] = []
                if trace_index == 0 and args.smoke_test:
                    phases.append(("smoke", 0))
                phases.extend(("warmup", trial) for trial in range(1, args.warmups + 1))
                phases.extend(("measure", trial) for trial in range(1, args.trials + 1))
                for phase, trial in phases:
                    for tool in ("co-checker", "timekiller"):
                        input_path = trace.path if tool == "co-checker" else json_path
                        command = checker_command(args, tool, input_path)
                        if tool == "timekiller" and not conversion_ok:
                            row = skipped_trial_row(
                                args,
                                trace,
                                tool=tool,
                                phase=phase,
                                trial=trial,
                                reason="conversion did not produce validated JSON",
                                command=command,
                            )
                        else:
                            row = run_checker_trial(
                                args,
                                trace,
                                tool=tool,
                                phase=phase,
                                trial=trial,
                                input_path=input_path,
                            )
                        trials.append(row)
                        print(
                            f"  {phase} {trial} {tool}: {row['outcome']}"
                            + (f" verdict={row['verdict']}" if row.get("verdict") else "")
                            + (
                                f" wall={format_number(row.get('wall_seconds'))}s"
                                if row.get("wall_seconds") is not None
                                else ""
                            ),
                            flush=True,
                        )
                        failed = row["outcome"] != "success"
                        trace_failed |= failed
                        any_run_failure |= failed

                cleanup_converted(
                    args,
                    json_path,
                    stats_path,
                    retain_for_failure=trace_failed,
                )
        except KeyboardInterrupt:
            interrupted = True
            any_run_failure = True
            print("interrupted; preserving completed CSV rows and generating a partial report", file=sys.stderr)

    summaries = write_summary(args, traces)
    write_report(args, traces, summaries)
    print(f"results: {args.result_dir}", flush=True)
    if interrupted:
        return 130
    return 1 if any_run_failure else 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = make_parser()
    args = parser.parse_args(argv)
    try:
        args.repo_root = args.repo_root.expanduser().resolve()
        validate_args(args)
        traces = discover_traces(args)
        if args.list_traces:
            print_traces(traces)
            return 0
        prepare_paths(args)
        return execute(args, traces)
    except ValueError as error:
        parser.error(str(error))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
