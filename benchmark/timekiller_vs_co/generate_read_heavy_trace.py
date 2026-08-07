#!/usr/bin/env python3
"""Generate deterministic, valid, read-heavy COTRACE1 histories.

The first transaction prepares every key with a non-null value. Remaining
transactions read committed values without modifying the database. This keeps
checker verdicts meaningful while making read density, key reuse, transaction
width, and binary payload size independently configurable.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
from typing import BinaryIO, Sequence


TRACE_HEADER = struct.Struct("<8sIIQQ")
TXN_HEADER = struct.Struct("<IQQI")
EVENT_HEADER = struct.Struct("<IBBH")
U32 = struct.Struct("<I")
TRACE_MAGIC = b"COTRACE1"
TRACE_VERSION = 1
TRACE_FLAGS = 0
HAS_VALUE = 1
READ = 0
WRITE = 1
MAX_U32 = (1 << 32) - 1
MAX_JAVA_LONG = (1 << 63) - 1


@dataclasses.dataclass(frozen=True)
class GenerationConfig:
    output: Path
    key_count: int
    read_transactions: int
    reads_per_transaction: int
    sessions: int
    key_bytes: int
    value_bytes: int
    history_rounds: int = 0
    repeated_writes: int = 1
    stats: Path | None = None

    @property
    def transactions(self) -> int:
        anchor_transactions = self.read_transactions if self.history_rounds else 0
        return 1 + anchor_transactions + self.history_rounds + self.read_transactions

    @property
    def read_events(self) -> int:
        return self.read_transactions * self.reads_per_transaction

    @property
    def events(self) -> int:
        return self.write_events + self.read_events

    @property
    def history_write_events(self) -> int:
        return self.history_rounds * self.key_count * self.repeated_writes

    @property
    def write_events(self) -> int:
        return self.key_count + self.history_write_events


def validate_config(config: GenerationConfig) -> None:
    if config.key_count <= 0:
        raise ValueError("--keys must be positive")
    if config.read_transactions <= 0:
        raise ValueError("--read-transactions must be positive")
    if config.reads_per_transaction <= 0:
        raise ValueError("--reads-per-transaction must be positive")
    if config.sessions <= 0:
        raise ValueError("--sessions must be positive")
    if config.history_rounds < 0:
        raise ValueError("--history-rounds must be non-negative")
    if config.repeated_writes <= 0:
        raise ValueError("--repeated-writes must be positive")
    if config.key_bytes < 8:
        raise ValueError("--key-bytes must be at least 8")
    if config.value_bytes < 8:
        raise ValueError("--value-bytes must be at least 8")
    if config.transactions > MAX_U32:
        raise ValueError("transaction count exceeds the checker's uint32 id range")
    if config.events > MAX_U32:
        raise ValueError("event count exceeds the checker's uint32 id range")
    if config.sessions > MAX_U32:
        raise ValueError("session count exceeds the binary uint32 range")
    if config.reads_per_transaction > MAX_U32:
        raise ValueError("reads per transaction exceed the binary uint32 range")
    if config.key_count * config.repeated_writes > MAX_U32:
        raise ValueError("writes per history transaction exceed the binary uint32 range")
    if config.history_rounds and config.read_transactions > MAX_U32 - 1:
        raise ValueError("stale-read mode requires one unique session per read transaction")
    final_commit = 2 + 2 * (
        config.read_transactions + config.history_rounds +
        (config.read_transactions if config.history_rounds else 0)
    )
    if final_commit > MAX_JAVA_LONG:
        raise ValueError("generated timestamps exceed signed Java long")

    output = config.output.resolve()
    if config.stats is not None and output == config.stats.resolve():
        raise ValueError("trace output and stats paths must be different")


def fixed_payload(identifier: int, size: int, domain: int) -> bytes:
    """Return a collision-free fixed-size payload for a uint64 identifier."""
    prefix = identifier.to_bytes(8, "big")
    if size == 8:
        return prefix
    tail = bytes(((domain + identifier * 17 + index * 29) & 0xFF) for index in range(size - 8))
    return prefix + tail


def append_event(buffer: bytearray, sequence: int, op: int, key: bytes, value: bytes) -> None:
    buffer.extend(EVENT_HEADER.pack(sequence, op, HAS_VALUE, 0))
    buffer.extend(U32.pack(len(key)))
    buffer.extend(key)
    buffer.extend(U32.pack(len(value)))
    buffer.extend(value)


def write_transaction(
    output: BinaryIO,
    session_id: int,
    start_ts: int,
    commit_ts: int,
    operations: Sequence[tuple[int, bytes, bytes]],
) -> None:
    output.write(TXN_HEADER.pack(session_id, start_ts, commit_ts, len(operations)))
    encoded = bytearray()
    for sequence, (op, key, value) in enumerate(operations):
        append_event(encoded, sequence, op, key, value)
    output.write(encoded)


def generate(config: GenerationConfig) -> dict[str, int | float | str]:
    validate_config(config)
    config.output.parent.mkdir(parents=True, exist_ok=True)
    if config.stats is not None:
        config.stats.parent.mkdir(parents=True, exist_ok=True)

    keys = [fixed_payload(key_id, config.key_bytes, 0x4B) for key_id in range(config.key_count)]
    values = [fixed_payload(key_id, config.value_bytes, 0x56) for key_id in range(config.key_count)]
    updated_values = (
        [
            fixed_payload(config.key_count + key_id, config.value_bytes, 0xA7)
            for key_id in range(config.key_count)
        ]
        if config.history_rounds
        else []
    )

    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w+b", prefix=f".{config.output.name}.", suffix=".tmp",
            dir=config.output.parent, delete=False,
        ) as output:
            temporary_name = output.name
            output.write(
                TRACE_HEADER.pack(
                    TRACE_MAGIC, TRACE_VERSION, TRACE_FLAGS,
                    config.transactions, config.events,
                )
            )

            prepare_operations = [
                (WRITE, keys[key_id], values[key_id]) for key_id in range(config.key_count)
            ]
            write_transaction(output, 0, 1, 2, prepare_operations)

            next_timestamp = 3
            anchor_commits: list[int] = []
            if config.history_rounds:
                for txn_index in range(config.read_transactions):
                    anchor_commits.append(next_timestamp + 1)
                    write_transaction(
                        output, txn_index + 1, next_timestamp,
                        next_timestamp + 1, [],
                    )
                    next_timestamp += 2

                history_operations = [
                    (WRITE, keys[key_id], updated_values[key_id])
                    for key_id in range(config.key_count)
                    for _ in range(config.repeated_writes)
                ]
                for _ in range(config.history_rounds):
                    write_transaction(
                        output, 0, next_timestamp, next_timestamp + 1,
                        history_operations,
                    )
                    next_timestamp += 2

            for txn_index in range(config.read_transactions):
                first_key = (txn_index * config.reads_per_transaction) % config.key_count
                read_operations = [
                    (
                        READ,
                        keys[(first_key + offset) % config.key_count],
                        values[(first_key + offset) % config.key_count],
                    )
                    for offset in range(config.reads_per_transaction)
                ]
                if config.history_rounds:
                    start_ts = anchor_commits[txn_index] + 1
                    session_id = txn_index + 1
                else:
                    start_ts = next_timestamp
                    session_id = 1 + txn_index % config.sessions
                write_transaction(
                    output,
                    session_id,
                    start_ts,
                    next_timestamp + 1,
                    read_operations,
                )
                next_timestamp += 2

            output.flush()
            os.fsync(output.fileno())

        os.replace(temporary_name, config.output)
        temporary_name = None
    finally:
        if temporary_name is not None:
            try:
                os.unlink(temporary_name)
            except FileNotFoundError:
                pass

    stats: dict[str, int | float | str] = {
        "format": "COTRACE1",
        "version": TRACE_VERSION,
        "transactions": config.transactions,
        "events": config.events,
        "read_events": config.read_events,
        "write_events": config.write_events,
        "history_write_events": config.history_write_events,
        "read_fraction": config.read_events / config.events,
        "keys": config.key_count,
        "distinct_nonnull_values": config.key_count * (2 if config.history_rounds else 1),
        "sessions": (config.read_transactions + 1) if config.history_rounds else (config.sessions + 1),
        "key_bytes": config.key_bytes,
        "value_bytes": config.value_bytes,
        "history_rounds": config.history_rounds,
        "repeated_writes": config.repeated_writes,
        "output_bytes": config.output.stat().st_size,
        "output": str(config.output.resolve()),
    }
    if config.stats is not None:
        stats_text = json.dumps(stats, indent=2, sort_keys=True) + "\n"
        stats_temporary: str | None = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", prefix=f".{config.stats.name}.",
                suffix=".tmp", dir=config.stats.parent, delete=False,
            ) as output:
                stats_temporary = output.name
                output.write(stats_text)
                output.flush()
                os.fsync(output.fileno())
            os.replace(stats_temporary, config.stats)
            stats_temporary = None
        finally:
            if stats_temporary is not None:
                try:
                    os.unlink(stats_temporary)
                except FileNotFoundError:
                    pass
    return stats


def parse_args(argv: Sequence[str] | None = None) -> GenerationConfig:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--keys", type=int, default=65_536)
    parser.add_argument("--read-transactions", type=int, default=19_500)
    parser.add_argument("--reads-per-transaction", type=int, default=256)
    parser.add_argument("--sessions", type=int, default=64)
    parser.add_argument("--key-bytes", type=int, default=48)
    parser.add_argument("--value-bytes", type=int, default=128)
    parser.add_argument(
        "--history-rounds", type=int, default=0,
        help="writer transactions placed between per-reader anchors and stale reads",
    )
    parser.add_argument(
        "--repeated-writes", type=int, default=1,
        help="writes of each key in every history transaction",
    )
    parser.add_argument("--stats", type=Path)
    args = parser.parse_args(argv)
    return GenerationConfig(
        output=args.output,
        key_count=args.keys,
        read_transactions=args.read_transactions,
        reads_per_transaction=args.reads_per_transaction,
        sessions=args.sessions,
        key_bytes=args.key_bytes,
        value_bytes=args.value_bytes,
        history_rounds=args.history_rounds,
        repeated_writes=args.repeated_writes,
        stats=args.stats,
    )


def main(argv: Sequence[str] | None = None) -> int:
    try:
        stats = generate(parse_args(argv))
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(json.dumps(stats, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
