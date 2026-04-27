#!/usr/bin/env python3
"""Preprocess TiDB transaction traces for the co-consistency checker.

The raw trace emitted by the instrumented TiDB is a YAML stream with one
document per finished transaction. This tool keeps only committed
transactions and orders them by commit timestamp.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, TextIO

try:
    import yaml
except ImportError as exc:  # pragma: no cover - exercised only without PyYAML.
    raise SystemExit(
        "PyYAML is required. Install it with `python3 -m pip install pyyaml`."
    ) from exc


COMMITTED_STATUS = "committed"
SESSION_ID_BASE = 1
TRACE_NAME_RE = re.compile(r"^\d+s-\d+t-\d+e(?:-(?P<suffix>.+))?$")
SESSION_PREFIX_RE = re.compile(r"^\d+s-(?P<suffix>.+)$")


class TracePreprocessError(ValueError):
    """Raised when an input trace document is malformed."""


class TraceDumper(yaml.SafeDumper):
    """YAML dumper that indents nested block sequences under mapping keys."""

    def increase_indent(self, flow: bool = False, indentless: bool = False) -> None:
        return super().increase_indent(flow, False)


@dataclass(frozen=True)
class CommittedTransaction:
    original_index: int
    commit_ts: int
    document: dict[str, Any]


@dataclass(frozen=True)
class TraceStats:
    sessions: int
    transactions: int
    events: int


def load_trace(path: Path) -> list[dict[str, Any]]:
    """Load a YAML trace stream as a list of transaction documents."""
    with path.open("r", encoding="utf-8") as input_file:
        docs = list(yaml.safe_load_all(input_file))

    transactions: list[dict[str, Any]] = []
    for index, doc in enumerate(docs, start=1):
        if doc is None:
            continue
        if not isinstance(doc, dict):
            raise TracePreprocessError(
                f"document {index}: expected a mapping, got {type(doc).__name__}"
            )
        transactions.append(doc)
    return transactions


def preprocess_documents(documents: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    """Return committed transactions sorted by ascending commit timestamp.

    Current traces mark committed transactions with `status: "committed"`.
    Older traces may not have `status`; those are treated as committed only
    when they contain a valid `commit_ts` and a `session_id`. Sessionless docs
    are internal TiDB transactions and are not part of the application trace.
    Session IDs are normalized per trace to a dense range starting at 1 because
    TiDB connection IDs are process-wide.
    """
    committed: list[CommittedTransaction] = []
    for original_index, document in enumerate(documents):
        commit_ts = _committed_commit_ts(document, original_index + 1)
        if commit_ts is None:
            continue
        if "session_id" not in document:
            continue
        normalized_document = dict(document)
        normalized_document.pop("status", None)
        _normalize_events(normalized_document)
        committed.append(
            CommittedTransaction(
                original_index=original_index,
                commit_ts=commit_ts,
                document=normalized_document,
            )
        )

    committed.sort(key=lambda txn: (txn.commit_ts, txn.original_index))
    sorted_documents = [txn.document for txn in committed]
    _normalize_session_ids(sorted_documents)
    return sorted_documents


def _normalize_events(document: dict[str, Any]) -> None:
    """Drop local-buffer probe reads and checker-irrelevant source metadata."""
    events = document.get("events")
    if not isinstance(events, list):
        return

    normalized_events: list[Any] = []
    for event in events:
        if not isinstance(event, dict):
            normalized_events.append(event)
            continue

        if event.get("op") == "R" and event.get("source") == "mem_buffer":
            continue

        normalized_event = dict(event)
        normalized_event.pop("source", None)
        normalized_events.append(normalized_event)

    document["events"] = normalized_events


def write_trace(documents: Iterable[dict[str, Any]], output_file: TextIO) -> None:
    """Write transactions as a YAML document stream."""
    yaml.dump_all(
        list(documents),
        output_file,
        Dumper=TraceDumper,
        explicit_start=True,
        sort_keys=False,
        default_flow_style=False,
    )


def preprocess_file(
    input_path: Path,
    output_path: Path | None,
    in_place: bool,
    rename: bool = False,
) -> Path | None:
    """Preprocess one trace file."""
    documents = preprocess_documents(load_trace(input_path))

    if in_place:
        _write_in_place(input_path, documents)
        if rename:
            return _rename_with_trace_stats(input_path, documents)
        return input_path

    if output_path is None:
        write_trace(documents, sys.stdout)
        return None

    final_output_path = (
        _path_with_trace_stats(output_path, documents) if rename else output_path
    )
    with output_path.open("w", encoding="utf-8") as output_file:
        write_trace(documents, output_file)
    if final_output_path != output_path:
        output_path.replace(final_output_path)
    return final_output_path


def trace_stats(documents: Iterable[dict[str, Any]]) -> TraceStats:
    documents = list(documents)
    sessions = {
        _parse_session_id(document["session_id"])
        for document in documents
        if "session_id" in document
    }
    events = 0
    for document in documents:
        document_events = document.get("events")
        if isinstance(document_events, list):
            events += len(document_events)
    return TraceStats(
        sessions=len(sessions),
        transactions=len(documents),
        events=events,
    )


def _committed_commit_ts(document: dict[str, Any], doc_number: int) -> int | None:
    status = document.get("status")
    commit_ts = document.get("commit_ts")

    if status is None:
        return _parse_optional_commit_ts(commit_ts, doc_number)

    if status != COMMITTED_STATUS:
        return None

    if commit_ts is None:
        raise TracePreprocessError(
            f"document {doc_number}: committed transaction is missing commit_ts"
        )
    return _parse_required_commit_ts(commit_ts, doc_number)


def _parse_optional_commit_ts(value: Any, doc_number: int) -> int | None:
    if value is None:
        return None
    return _parse_required_commit_ts(value, doc_number)


def _parse_required_commit_ts(value: Any, doc_number: int) -> int:
    if isinstance(value, bool):
        raise TracePreprocessError(
            f"document {doc_number}: commit_ts must be an unsigned integer"
        )

    if isinstance(value, int):
        commit_ts = value
    elif isinstance(value, str) and value.isdecimal():
        commit_ts = int(value)
    else:
        raise TracePreprocessError(
            f"document {doc_number}: commit_ts must be an unsigned integer"
        )

    if commit_ts <= 0:
        raise TracePreprocessError(
            f"document {doc_number}: commit_ts must be greater than zero"
        )
    return commit_ts


def _normalize_session_ids(documents: list[dict[str, Any]]) -> None:
    original_ids = sorted(
        {
            _parse_session_id(document["session_id"])
            for document in documents
            if "session_id" in document
        }
    )
    session_id_map = {
        session_id: index + SESSION_ID_BASE
        for index, session_id in enumerate(original_ids)
    }

    for document in documents:
        if "session_id" not in document:
            continue
        document["session_id"] = session_id_map[
            _parse_session_id(document["session_id"])
        ]


def _parse_session_id(value: Any) -> int:
    if isinstance(value, bool):
        raise TracePreprocessError("session_id must be an unsigned integer")

    if isinstance(value, int):
        session_id = value
    elif isinstance(value, str) and value.isdecimal():
        session_id = int(value)
    else:
        raise TracePreprocessError("session_id must be an unsigned integer")

    if session_id < 0:
        raise TracePreprocessError("session_id must be an unsigned integer")
    return session_id


def _write_in_place(path: Path, documents: list[dict[str, Any]]) -> None:
    original_mode = path.stat().st_mode & 0o777
    with tempfile.NamedTemporaryFile(
        "w",
        encoding="utf-8",
        dir=path.parent,
        prefix=f".{path.name}.",
        suffix=".tmp",
        delete=False,
    ) as temp_file:
        temp_path = Path(temp_file.name)
        write_trace(documents, temp_file)

    temp_path.chmod(original_mode)
    temp_path.replace(path)


def _rename_with_trace_stats(path: Path, documents: list[dict[str, Any]]) -> Path:
    renamed_path = _path_with_trace_stats(path, documents)
    if renamed_path == path:
        return path
    path.replace(renamed_path)
    return renamed_path


def _path_with_trace_stats(path: Path, documents: list[dict[str, Any]]) -> Path:
    stats = trace_stats(documents)
    suffix = _trace_name_suffix(path.stem)
    stem = f"{stats.sessions}s-{stats.transactions}t-{stats.events}e"
    if suffix:
        stem = f"{stem}-{suffix}"
    return path.with_name(f"{stem}{path.suffix}")


def _trace_name_suffix(stem: str) -> str:
    match = TRACE_NAME_RE.fullmatch(stem)
    if match is not None:
        return match.group("suffix") or ""

    match = SESSION_PREFIX_RE.fullmatch(stem)
    if match is not None:
        return match.group("suffix")

    return stem


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Remove uncommitted transactions from a TiDB trace YAML stream and "
            "sort committed transactions by commit_ts."
        )
    )
    parser.add_argument("input", type=Path, help="raw TiDB trace YAML file")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="output YAML file; defaults to stdout unless --in-place is used",
    )
    parser.add_argument(
        "--in-place",
        action="store_true",
        help="overwrite the input trace with the preprocessed result",
    )
    parser.add_argument(
        "--rename",
        action="store_true",
        help=(
            "rename the output to <sessions>s-<transactions>t-<events>e[-suffix].yaml "
            "using the preprocessed trace contents"
        ),
    )
    args = parser.parse_args(argv)

    if args.in_place and args.output is not None:
        parser.error("--in-place cannot be combined with --output")
    if args.rename and not args.in_place and args.output is None:
        parser.error("--rename requires --in-place or --output")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        output_path = preprocess_file(args.input, args.output, args.in_place, args.rename)
        if args.rename and output_path is not None:
            print(output_path)
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except TracePreprocessError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
