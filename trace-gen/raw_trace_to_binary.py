#!/usr/bin/env python3
"""Convert an instrumented-TiDB YAML stream directly to COTRACE1.

Unlike ``trace-preprocess`` followed by ``trace-convert``, this converter does
not retain all events in memory.  It makes two passes over the raw trace:

* scan one YAML document at a time and retain only transaction metadata;
* sort that small metadata index, seek back to each document, and stream its
  normalized events into the binary output.

Peak memory is therefore the metadata index plus one transaction.  The TiDB
trace sink writes every document with an explicit ``---`` marker, which makes
the byte offsets stable and cheap to discover.
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, BinaryIO, Iterator

try:
    import yaml
except ImportError as exc:  # pragma: no cover - only exercised without PyYAML.
    raise SystemExit(
        "PyYAML is required. Install it with `python3 -m pip install pyyaml`."
    ) from exc


TRACE_MAGIC = b"COTRACE1"
TRACE_VERSION = 1
TRACE_FLAGS = 0
COMMITTED_STATUS = "committed"

UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1

EVENT_HAS_VALUE = 1 << 0
EVENT_DELETE = 1 << 1
EVENT_MISS = 1 << 2

TRACE_HEADER = struct.Struct("<8sIIQQ")
TXN_HEADER = struct.Struct("<IQQI")
EVENT_HEADER = struct.Struct("<IBBH")
BYTE_LENGTH = struct.Struct("<I")

TRACE_NAME_RE = re.compile(r"^\d+s-\d+t-\d+e(?:-(?P<suffix>.+))?$")
SESSION_PREFIX_RE = re.compile(r"^\d+s-(?P<suffix>.+)$")
HEX_RE = re.compile(r"^[0-9A-Fa-f]*$")

YAML_LOADER = getattr(yaml, "CSafeLoader", yaml.SafeLoader)


class RawTraceConversionError(ValueError):
    """Raised when a raw transaction document cannot be converted."""


@dataclass(frozen=True)
class TransactionMetadata:
    original_index: int
    offset: int
    length: int
    session_id: int
    start_ts: int
    commit_ts: int
    event_count: int


@dataclass(frozen=True)
class ParsedEvent:
    seq: int
    op: int
    key: bytes
    value: bytes | None
    is_delete: bool
    is_miss: bool


@dataclass(frozen=True)
class ParsedTransaction:
    session_id: int
    start_ts: int
    commit_ts: int
    events: tuple[ParsedEvent, ...]


@dataclass(frozen=True)
class TraceStats:
    sessions: int
    transactions: int
    events: int


@dataclass(frozen=True)
class FileIdentity:
    device: int
    inode: int
    size: int
    mtime_ns: int


def _file_identity(path: Path) -> FileIdentity:
    stat = path.stat()
    return FileIdentity(stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns)


def _document_ranges(input_file: BinaryIO) -> Iterator[tuple[int, int, int]]:
    """Yield ``(zero_based_index, offset, length)`` for TiDB YAML docs."""
    document_start: int | None = None
    document_index = 0
    leading_non_whitespace = False

    while True:
        line_start = input_file.tell()
        line = input_file.readline()
        if not line:
            break

        is_marker = line in (b"---\n", b"---\r\n", b"---")
        if is_marker:
            if document_start is not None:
                yield document_index, document_start, line_start - document_start
                document_index += 1
            elif leading_non_whitespace:
                raise RawTraceConversionError(
                    "input contains data before its first YAML document marker"
                )
            document_start = line_start
        elif document_start is None and line.strip():
            leading_non_whitespace = True

    end = input_file.tell()
    if document_start is not None:
        yield document_index, document_start, end - document_start
    elif leading_non_whitespace:
        raise RawTraceConversionError(
            "input is not an instrumented-TiDB YAML stream: no `---` marker found"
        )


def _read_exact_range(input_file: BinaryIO, offset: int, length: int) -> bytes:
    input_file.seek(offset)
    data = input_file.read(length)
    if len(data) != length:
        raise RawTraceConversionError(
            f"raw trace changed or was truncated while reading offset {offset}"
        )
    return data


def _load_document(data: bytes, document_number: int) -> dict[str, Any] | None:
    try:
        document = yaml.load(data, Loader=YAML_LOADER)
    except yaml.YAMLError as exc:
        raise RawTraceConversionError(
            f"document {document_number}: invalid YAML: {exc}"
        ) from exc

    if document is None:
        return None
    if not isinstance(document, dict):
        raise RawTraceConversionError(
            f"document {document_number}: expected a mapping"
        )
    return document


def _unsigned(value: Any, field: str, document_number: int, maximum: int) -> int:
    if isinstance(value, bool):
        parsed: int | None = None
    elif isinstance(value, int):
        parsed = value
    elif isinstance(value, str) and value.isdecimal():
        parsed = int(value)
    else:
        parsed = None

    if parsed is None or parsed < 0 or parsed > maximum:
        width = 32 if maximum == UINT32_MAX else 64
        raise RawTraceConversionError(
            f"document {document_number}: {field} must be a uint{width}"
        )
    return parsed


def _boolean(value: Any, field: str, document_number: int, event_number: int) -> bool:
    if not isinstance(value, bool):
        raise RawTraceConversionError(
            f"document {document_number}, event {event_number}: {field} must be a boolean"
        )
    return value


def _hex_bytes(
    value: Any, field: str, document_number: int, event_number: int
) -> bytes:
    if not isinstance(value, str):
        raise RawTraceConversionError(
            f"document {document_number}, event {event_number}: {field} must be a scalar string"
        )
    if len(value) % 2 != 0:
        raise RawTraceConversionError(
            f"document {document_number}, event {event_number}: {field} has odd length"
        )
    if HEX_RE.fullmatch(value) is None:
        raise RawTraceConversionError(
            f"document {document_number}, event {event_number}: {field} contains a non-hex character"
        )
    byte_length = len(value) // 2
    if byte_length > UINT32_MAX:
        raise RawTraceConversionError(
            f"document {document_number}, event {event_number}: {field} is too large"
        )
    return bytes.fromhex(value)


def _parse_events(document: dict[str, Any], document_number: int) -> tuple[ParsedEvent, ...]:
    raw_events = document.get("events")
    if not isinstance(raw_events, list):
        return ()

    events: list[ParsedEvent] = []
    previous_seq: int | None = None
    for raw_event_number, raw_event in enumerate(raw_events, start=1):
        if not isinstance(raw_event, dict):
            raise RawTraceConversionError(
                f"document {document_number}, event {raw_event_number}: expected a mapping"
            )

        # Local reads only observe this transaction's own writes.  They are not
        # part of the checker history; writes keep their source-independent form.
        if raw_event.get("op") == "R" and raw_event.get("source") == "mem_buffer":
            continue

        for required in ("seq", "op", "key_hex"):
            if required not in raw_event:
                raise RawTraceConversionError(
                    f"document {document_number}, event {raw_event_number}: missing {required}"
                )

        seq = _unsigned(
            raw_event["seq"], "seq", document_number, UINT32_MAX
        )
        if previous_seq is not None and seq <= previous_seq:
            raise RawTraceConversionError(
                f"document {document_number}, event {raw_event_number}: seq must be strictly increasing"
            )
        previous_seq = seq

        raw_op = raw_event["op"]
        if raw_op == "R":
            op = 0
        elif raw_op == "W":
            op = 1
        else:
            raise RawTraceConversionError(
                f"document {document_number}, event {raw_event_number}: op must be `R` or `W`"
            )

        key = _hex_bytes(
            raw_event["key_hex"], "key_hex", document_number, raw_event_number
        )
        value = None
        if "value_hex" in raw_event:
            value = _hex_bytes(
                raw_event["value_hex"],
                "value_hex",
                document_number,
                raw_event_number,
            )

        is_delete = (
            _boolean(
                raw_event["delete"], "delete", document_number, raw_event_number
            )
            if "delete" in raw_event
            else False
        )
        is_miss = (
            _boolean(raw_event["miss"], "miss", document_number, raw_event_number)
            if "miss" in raw_event
            else False
        )

        if is_miss and value is not None:
            raise RawTraceConversionError(
                f"document {document_number}, event {raw_event_number}: cannot have both miss and value_hex"
            )
        if is_delete and value is not None:
            raise RawTraceConversionError(
                f"document {document_number}, event {raw_event_number}: cannot have both delete and value_hex"
            )
        if op == 0 and is_delete:
            raise RawTraceConversionError(
                f"document {document_number}, event {raw_event_number}: read cannot have delete"
            )
        if op == 1 and is_miss:
            raise RawTraceConversionError(
                f"document {document_number}, event {raw_event_number}: write cannot have miss"
            )

        events.append(
            ParsedEvent(
                seq=seq,
                op=op,
                key=key,
                value=value,
                is_delete=is_delete,
                is_miss=is_miss,
            )
        )

    if len(events) > UINT32_MAX:
        raise RawTraceConversionError(
            f"document {document_number}: transaction has too many events"
        )
    return tuple(events)


def _parse_transaction(
    data: bytes, document_number: int
) -> ParsedTransaction | None:
    document = _load_document(data, document_number)
    if document is None:
        return None

    status = document.get("status")
    if status is not None and status != COMMITTED_STATUS:
        return None

    if "commit_ts" not in document:
        if status == COMMITTED_STATUS:
            raise RawTraceConversionError(
                f"document {document_number}: committed transaction is missing commit_ts"
            )
        return None
    if "session_id" not in document:
        return None
    if "start_ts" not in document:
        raise RawTraceConversionError(
            f"document {document_number}: committed transaction is missing start_ts"
        )

    session_id = _unsigned(
        document["session_id"], "session_id", document_number, UINT64_MAX
    )
    start_ts = _unsigned(
        document["start_ts"], "start_ts", document_number, UINT64_MAX
    )
    commit_ts = _unsigned(
        document["commit_ts"], "commit_ts", document_number, UINT64_MAX
    )
    events = _parse_events(document, document_number)
    return ParsedTransaction(session_id, start_ts, commit_ts, events)


def scan_trace(input_path: Path) -> tuple[list[TransactionMetadata], TraceStats]:
    transactions: list[TransactionMetadata] = []
    session_ids: set[int] = set()
    event_count = 0

    # Keep range discovery and document reads on separate handles: the range
    # generator is suspended at each yield and must retain its sequential file
    # position while the other handle seeks to the completed document.
    with input_path.open("rb") as range_file, input_path.open("rb") as input_file:
        for original_index, offset, length in _document_ranges(range_file):
            data = _read_exact_range(input_file, offset, length)
            transaction = _parse_transaction(data, original_index + 1)
            if transaction is None:
                continue

            transaction_events = len(transaction.events)
            if event_count > UINT32_MAX - transaction_events:
                raise RawTraceConversionError(
                    "trace exceeds the COTRACE1 event ID range"
                )
            event_count += transaction_events
            session_ids.add(transaction.session_id)
            transactions.append(
                TransactionMetadata(
                    original_index=original_index,
                    offset=offset,
                    length=length,
                    session_id=transaction.session_id,
                    start_ts=transaction.start_ts,
                    commit_ts=transaction.commit_ts,
                    event_count=transaction_events,
                )
            )

    if len(transactions) > UINT32_MAX:
        raise RawTraceConversionError(
            "trace exceeds the COTRACE1 transaction ID range"
        )
    if len(session_ids) > UINT32_MAX:
        raise RawTraceConversionError(
            "trace exceeds the COTRACE1 session ID range"
        )

    transactions.sort(key=lambda txn: (txn.commit_ts, txn.original_index))
    for previous, current in zip(transactions, transactions[1:]):
        if previous.commit_ts == current.commit_ts:
            raise RawTraceConversionError(
                f"document {current.original_index + 1}: commit_ts {current.commit_ts} "
                "duplicates an earlier transaction"
            )

    return transactions, TraceStats(
        sessions=len(session_ids),
        transactions=len(transactions),
        events=event_count,
    )


def _write_byte_string(output: BinaryIO, value: bytes) -> None:
    output.write(BYTE_LENGTH.pack(len(value)))
    output.write(value)


def _write_event(output: BinaryIO, event: ParsedEvent) -> None:
    flags = 0
    if event.value is not None:
        flags |= EVENT_HAS_VALUE
    if event.is_delete:
        flags |= EVENT_DELETE
    if event.is_miss:
        flags |= EVENT_MISS
    output.write(EVENT_HEADER.pack(event.seq, event.op, flags, 0))
    _write_byte_string(output, event.key)
    if event.value is not None:
        _write_byte_string(output, event.value)


def _session_id_map(transactions: list[TransactionMetadata]) -> dict[int, int]:
    return {
        session_id: dense_id
        for dense_id, session_id in enumerate(
            sorted({txn.session_id for txn in transactions}), start=1
        )
    }


def _trace_name_suffix(stem: str) -> str:
    match = TRACE_NAME_RE.fullmatch(stem)
    if match is not None:
        return match.group("suffix") or ""
    match = SESSION_PREFIX_RE.fullmatch(stem)
    if match is not None:
        return match.group("suffix")
    return stem


def path_with_stats(path: Path, stats: TraceStats) -> Path:
    stem = f"{stats.sessions}s-{stats.transactions}t-{stats.events}e"
    suffix = _trace_name_suffix(path.stem)
    if suffix:
        stem = f"{stem}-{suffix}"
    return path.with_name(f"{stem}.trace")


def default_output_path(input_path: Path) -> Path:
    return input_path.with_suffix(".trace")


def convert_trace(
    input_path: Path, output_path: Path | None = None, rename: bool = False
) -> Path:
    input_path = input_path.resolve()
    initial_identity = _file_identity(input_path)
    transactions, stats = scan_trace(input_path)
    if _file_identity(input_path) != initial_identity:
        raise RawTraceConversionError("raw trace changed during the metadata scan")

    destination = (output_path or default_output_path(input_path)).resolve()
    if rename:
        destination = path_with_stats(destination, stats)
    if destination == input_path:
        raise RawTraceConversionError("binary output path must differ from raw input")
    destination.parent.mkdir(parents=True, exist_ok=True)

    session_ids = _session_id_map(transactions)
    temp_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            "wb",
            dir=destination.parent,
            prefix=f".{destination.name}.",
            suffix=".tmp",
            delete=False,
        ) as output:
            temp_path = Path(output.name)
            output.write(
                TRACE_HEADER.pack(
                    TRACE_MAGIC,
                    TRACE_VERSION,
                    TRACE_FLAGS,
                    stats.transactions,
                    stats.events,
                )
            )

            with input_path.open("rb") as input_file:
                for metadata in transactions:
                    data = _read_exact_range(
                        input_file, metadata.offset, metadata.length
                    )
                    transaction = _parse_transaction(
                        data, metadata.original_index + 1
                    )
                    if transaction is None:
                        raise RawTraceConversionError(
                            f"document {metadata.original_index + 1}: raw trace changed between passes"
                        )
                    observed = (
                        transaction.session_id,
                        transaction.start_ts,
                        transaction.commit_ts,
                        len(transaction.events),
                    )
                    expected = (
                        metadata.session_id,
                        metadata.start_ts,
                        metadata.commit_ts,
                        metadata.event_count,
                    )
                    if observed != expected:
                        raise RawTraceConversionError(
                            f"document {metadata.original_index + 1}: raw trace changed between passes"
                        )

                    output.write(
                        TXN_HEADER.pack(
                            session_ids[transaction.session_id],
                            transaction.start_ts,
                            transaction.commit_ts,
                            len(transaction.events),
                        )
                    )
                    for event in transaction.events:
                        _write_event(output, event)

            output.flush()
            os.fsync(output.fileno())

        if _file_identity(input_path) != initial_identity:
            raise RawTraceConversionError("raw trace changed during conversion")
        os.replace(temp_path, destination)
        temp_path = None
        return destination
    finally:
        if temp_path is not None:
            try:
                temp_path.unlink()
            except FileNotFoundError:
                pass


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Filter and sort an instrumented-TiDB raw YAML trace, then write "
            "COTRACE1 without materializing the complete trace in memory."
        )
    )
    parser.add_argument("input", type=Path, help="raw TiDB YAML trace")
    parser.add_argument("-o", "--output", type=Path, help="output .trace path")
    parser.add_argument(
        "--rename",
        action="store_true",
        help=(
            "name the result <sessions>s-<transactions>t-<events>e[-suffix].trace"
        ),
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        output_path = convert_trace(args.input, args.output, args.rename)
    except (OSError, RawTraceConversionError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(output_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
