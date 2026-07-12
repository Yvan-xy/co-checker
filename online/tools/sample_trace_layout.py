#!/usr/bin/env python3
"""Stream legacy binary traces and evaluate packed V1 chunk policies."""

from __future__ import annotations

import argparse
import json
import math
import os
import struct
import time
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterable


TRACE_MAGIC = b"COTRACE1"
TRACE_VERSION = 1
PACKED_HEADER_BYTES = 168
PACKED_DESCRIPTOR_BYTES = 32
HAS_VALUE = 1 << 0
DELETE = 1 << 1
MISS = 1 << 2
KNOWN_FLAGS = HAS_VALUE | DELETE | MISS


class TraceFormatError(RuntimeError):
    pass


def read_exact(stream: BinaryIO, size: int) -> bytes:
    value = stream.read(size)
    if len(value) != size:
        raise TraceFormatError(f"unexpected EOF: needed {size} bytes")
    return value


def read_struct(stream: BinaryIO, layout: str) -> tuple[int, ...]:
    size = struct.calcsize(layout)
    return struct.unpack(layout, read_exact(stream, size))


def read_blob_size(stream: BinaryIO) -> int:
    (size,) = read_struct(stream, "<I")
    if size:
        read_exact(stream, size)
    return size


@dataclass(frozen=True)
class EventShape:
    key_bytes: int
    value_bytes: int
    has_value: bool
    is_read: bool


@dataclass
class TransactionShape:
    event_count: int = 0
    read_count: int = 0
    write_count: int = 0
    payload_bytes: int = 0
    max_key_bytes: int = 0
    max_value_bytes: int = 0
    max_record_bytes: int = 0
    events: list[EventShape] | None = None


@dataclass(frozen=True)
class ChunkLayout:
    used_bytes: int
    reserved_bytes: int
    descriptor_segments: int
    payload_segments: int
    dedicated_segments: int


def iter_trace(path: Path) -> tuple[dict[str, int], Iterable[TransactionShape]]:
    stream = path.open("rb")
    try:
        magic = read_exact(stream, len(TRACE_MAGIC))
        version, flags, transaction_count, event_count = read_struct(
            stream, "<IIQQ"
        )
        if magic != TRACE_MAGIC:
            raise TraceFormatError("invalid trace magic")
        if version != TRACE_VERSION or flags != 0:
            raise TraceFormatError("unsupported trace version or flags")
    except Exception:
        stream.close()
        raise

    header = {
        "transaction_count": transaction_count,
        "event_count": event_count,
    }

    def transactions() -> Iterable[TransactionShape]:
        observed_events = 0
        previous_commit_ts: int | None = None
        try:
            for transaction_index in range(transaction_count):
                _session_id, _start_ts, commit_ts, txn_event_count = read_struct(
                    stream, "<IQQI"
                )
                if previous_commit_ts is not None and commit_ts <= previous_commit_ts:
                    raise TraceFormatError(
                        f"commit token is not unique/increasing at transaction "
                        f"{transaction_index}"
                    )
                previous_commit_ts = commit_ts
                shape = TransactionShape(events=[])
                previous_sequence: int | None = None
                for event_index in range(txn_event_count):
                    sequence, operation, event_flags, reserved = read_struct(
                        stream, "<IBBH"
                    )
                    if previous_sequence is not None and sequence <= previous_sequence:
                        raise TraceFormatError(
                            f"event sequence is not increasing at transaction "
                            f"{transaction_index}, event {event_index}"
                        )
                    previous_sequence = sequence
                    if reserved != 0 or event_flags & ~KNOWN_FLAGS:
                        raise TraceFormatError("invalid event flags or reserved field")
                    if operation not in (0, 1):
                        raise TraceFormatError("invalid event operation")

                    has_value = bool(event_flags & HAS_VALUE)
                    is_delete = bool(event_flags & DELETE)
                    is_miss = bool(event_flags & MISS)
                    is_read = operation == 0
                    if is_read:
                        if is_delete or has_value == is_miss:
                            raise TraceFormatError(
                                "read must have exactly one of value or miss"
                            )
                    elif is_miss or has_value == is_delete:
                        raise TraceFormatError(
                            "write must have exactly one of value or delete"
                        )

                    key_bytes = read_blob_size(stream)
                    value_bytes = read_blob_size(stream) if has_value else 0
                    event = EventShape(
                        key_bytes=key_bytes,
                        value_bytes=value_bytes,
                        has_value=has_value,
                        is_read=is_read,
                    )
                    shape.events.append(event)
                    shape.event_count += 1
                    shape.read_count += int(is_read)
                    shape.write_count += int(not is_read)
                    shape.payload_bytes += key_bytes + value_bytes
                    shape.max_key_bytes = max(shape.max_key_bytes, key_bytes)
                    shape.max_value_bytes = max(shape.max_value_bytes, value_bytes)
                    shape.max_record_bytes = max(
                        shape.max_record_bytes,
                        PACKED_DESCRIPTOR_BYTES + key_bytes + value_bytes,
                    )
                observed_events += shape.event_count
                yield shape

            if observed_events != event_count:
                raise TraceFormatError(
                    f"header has {event_count} events, decoded {observed_events}"
                )
            if stream.read(1):
                raise TraceFormatError("trailing bytes after final transaction")
        finally:
            stream.close()

    return header, transactions()


def layout_for_chunk_size(shape: TransactionShape, chunk_size: int) -> ChunkLayout:
    if chunk_size < PACKED_DESCRIPTOR_BYTES:
        raise ValueError("chunk size must fit one descriptor")
    descriptor_segments = math.ceil(
        shape.event_count * PACKED_DESCRIPTOR_BYTES / chunk_size
    )
    payload_segments: list[list[int | bool]] = []
    dedicated_segments = 0

    def ensure_ordinary_space(size: int) -> None:
        if (
            not payload_segments
            or bool(payload_segments[-1][2])
            or int(payload_segments[-1][0]) - int(payload_segments[-1][1]) < size
        ):
            payload_segments.append([chunk_size, 0, False])

    def store_blob(size: int) -> None:
        nonlocal dedicated_segments
        if size == 0:
            return
        if size > chunk_size:
            payload_segments.append([size, size, True])
            dedicated_segments += 1
            return
        ensure_ordinary_space(size)
        payload_segments[-1][1] = int(payload_segments[-1][1]) + size

    assert shape.events is not None
    for event in shape.events:
        value_size = event.value_bytes if event.has_value else 0
        combined = event.key_bytes + value_size
        if 0 < combined <= chunk_size:
            ensure_ordinary_space(combined)
            payload_segments[-1][1] = int(payload_segments[-1][1]) + combined
        else:
            store_blob(event.key_bytes)
            if event.has_value:
                store_blob(value_size)

    used = (
        PACKED_HEADER_BYTES
        + shape.event_count * PACKED_DESCRIPTOR_BYTES
        + shape.payload_bytes
    )
    reserved = (
        PACKED_HEADER_BYTES
        + descriptor_segments * chunk_size
        + sum(int(segment[0]) for segment in payload_segments)
    )
    return ChunkLayout(
        used_bytes=used,
        reserved_bytes=reserved,
        descriptor_segments=descriptor_segments,
        payload_segments=len(payload_segments),
        dedicated_segments=dedicated_segments,
    )


def percentile(sorted_values: list[int], percent: float) -> int:
    if not sorted_values:
        return 0
    rank = math.ceil((percent / 100.0) * len(sorted_values))
    return sorted_values[max(0, min(len(sorted_values) - 1, rank - 1))]


def distribution(values: list[int]) -> dict[str, float | int]:
    ordered = sorted(values)
    total = sum(ordered)
    return {
        "count": len(ordered),
        "min": ordered[0] if ordered else 0,
        "p50": percentile(ordered, 50),
        "p90": percentile(ordered, 90),
        "p95": percentile(ordered, 95),
        "p99": percentile(ordered, 99),
        "p999": percentile(ordered, 99.9),
        "max": ordered[-1] if ordered else 0,
        "mean": total / len(ordered) if ordered else 0.0,
        "total": total,
    }


def sample(path: Path, chunk_sizes: list[int]) -> dict[str, object]:
    started = time.monotonic()
    header, transactions = iter_trace(path)
    event_counts: list[int] = []
    read_counts: list[int] = []
    write_counts: list[int] = []
    payload_bytes: list[int] = []
    used_bytes: list[int] = []
    maximum_key = 0
    maximum_value = 0
    maximum_record = 0
    policy_samples: dict[int, list[ChunkLayout]] = {
        size: [] for size in chunk_sizes
    }

    for shape in transactions:
        event_counts.append(shape.event_count)
        read_counts.append(shape.read_count)
        write_counts.append(shape.write_count)
        payload_bytes.append(shape.payload_bytes)
        used_bytes.append(
            PACKED_HEADER_BYTES
            + shape.event_count * PACKED_DESCRIPTOR_BYTES
            + shape.payload_bytes
        )
        maximum_key = max(maximum_key, shape.max_key_bytes)
        maximum_value = max(maximum_value, shape.max_value_bytes)
        maximum_record = max(maximum_record, shape.max_record_bytes)
        for size in chunk_sizes:
            policy_samples[size].append(layout_for_chunk_size(shape, size))

    policies: list[dict[str, object]] = []
    for size in chunk_sizes:
        layouts = policy_samples[size]
        total_used = sum(layout.used_bytes for layout in layouts)
        total_reserved = sum(layout.reserved_bytes for layout in layouts)
        policies.append(
            {
                "ordinary_chunk_bytes": size,
                "used_bytes": total_used,
                "reserved_bytes": total_reserved,
                "tail_slack_bytes": total_reserved - total_used,
                "reserved_over_used": (
                    total_reserved / total_used if total_used else 1.0
                ),
                "descriptor_segments": sum(
                    layout.descriptor_segments for layout in layouts
                ),
                "payload_segments": sum(
                    layout.payload_segments for layout in layouts
                ),
                "dedicated_segments": sum(
                    layout.dedicated_segments for layout in layouts
                ),
                "reserved_bytes_per_transaction": distribution(
                    [layout.reserved_bytes for layout in layouts]
                ),
            }
        )

    return {
        "format": "co-checker-packed-layout-sample-v1",
        "input": {
            "path": str(path.resolve()),
            "size_bytes": path.stat().st_size,
            **header,
        },
        "packed_layout": {
            "header_bytes": PACKED_HEADER_BYTES,
            "descriptor_bytes": PACKED_DESCRIPTOR_BYTES,
        },
        "transactions": {
            "events": distribution(event_counts),
            "reads": distribution(read_counts),
            "writes": distribution(write_counts),
            "payload_bytes": distribution(payload_bytes),
            "sealed_used_bytes": distribution(used_bytes),
            "maximum_key_bytes": maximum_key,
            "maximum_value_bytes": maximum_value,
            "maximum_record_bytes": maximum_record,
        },
        "chunk_policies": policies,
        "sampling_seconds": time.monotonic() - started,
        "limitations": [
            "committed offline traces do not expose capture peaks, retries, or rollback slack"
        ],
    }


def parse_chunk_sizes(raw: str) -> list[int]:
    values = sorted({int(value) for value in raw.split(",") if value})
    if not values or any(value < PACKED_DESCRIPTOR_BYTES for value in values):
        raise argparse.ArgumentTypeError(
            f"chunk sizes must be integers >= {PACKED_DESCRIPTOR_BYTES}"
        )
    return values


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--chunk-sizes",
        type=parse_chunk_sizes,
        default=parse_chunk_sizes("1024,2048,4096,8192,16384,32768,65536"),
    )
    args = parser.parse_args()

    report = sample(args.trace, args.chunk_sizes)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8") as output:
        json.dump(report, output, indent=2, sort_keys=True)
        output.write("\n")
    os.replace(temporary, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
