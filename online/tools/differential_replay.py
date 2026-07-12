#!/usr/bin/env python3
"""Compare the online engine with the unchanged offline SOWW executable."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import time
from pathlib import Path


def run(command: list[str]) -> dict[str, object]:
    started = time.monotonic()
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    return {
        "command": command,
        "exit_code": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
        "elapsed_seconds": time.monotonic() - started,
    }


def online_status(result: dict[str, object]) -> str:
    code = result["exit_code"]
    if code == 0:
        return "Pass"
    if code == 2:
        return "Violation"
    return "Error"


def offline_status(result: dict[str, object]) -> str:
    output = str(result["stdout"]) + str(result["stderr"])
    if "snapshot-so-ww: pass" in output and result["exit_code"] == 0:
        return "Pass"
    if "snapshot-so-ww: fail" in output:
        return "Violation"
    return "Error"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace-dir", type=Path, required=True)
    parser.add_argument("--trace-convert", type=Path, required=True)
    parser.add_argument("--offline-checker", type=Path, required=True)
    parser.add_argument("--online-replay", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    traces = sorted(args.trace_dir.glob("*.yaml"))
    if not traces:
        parser.error("trace directory contains no top-level YAML fixtures")
    args.work_dir.mkdir(parents=True, exist_ok=True)

    records: list[dict[str, object]] = []
    mismatch_count = 0
    error_count = 0
    for trace in traces:
        packed = args.work_dir / (trace.stem + ".trace")
        convert = run(
            [str(args.trace_convert), "-o", str(packed), str(trace)]
        )
        record: dict[str, object] = {
            "trace": str(trace.resolve()),
            "yaml_sha256": sha256(trace),
            "convert": convert,
        }
        if convert["exit_code"] != 0:
            record["classification"] = "ConverterError"
            error_count += 1
            records.append(record)
            continue

        online = run([str(args.online_replay), str(packed)])
        offline = run(
            [
                str(args.offline_checker),
                "--format",
                "bin",
                "--checker",
                "snapshot-so-ww",
                str(packed),
            ]
        )
        online_verdict = online_status(online)
        offline_verdict = offline_status(offline)
        if "Error" in (online_verdict, offline_verdict):
            classification = "ExecutionError"
            error_count += 1
        elif online_verdict != offline_verdict:
            classification = "SemanticMismatch"
            mismatch_count += 1
        else:
            classification = "Agreement"
        record.update(
            {
                "binary_sha256": sha256(packed),
                "online": online,
                "offline": offline,
                "online_verdict": online_verdict,
                "offline_verdict": offline_verdict,
                "classification": classification,
            }
        )
        records.append(record)

    report = {
        "format": "co-checker-online-offline-differential-v1",
        "trace_count": len(records),
        "agreement_count": sum(
            record.get("classification") == "Agreement" for record in records
        ),
        "mismatch_count": mismatch_count,
        "error_count": error_count,
        "scope_note": (
            "The legacy executable is an oracle only on overlapping semantics; "
            "any mismatch requires witness investigation rather than automatic "
            "classification as a checker bug."
        ),
        "records": records,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8") as output:
        json.dump(report, output, indent=2, sort_keys=True)
        output.write("\n")
    os.replace(temporary, args.output)

    print(
        f"traces={len(records)} agreements={report['agreement_count']} "
        f"mismatches={mismatch_count} errors={error_count}"
    )
    return 0 if mismatch_count == 0 and error_count == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
