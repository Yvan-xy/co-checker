from __future__ import annotations

import dataclasses
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import generate_read_heavy_trace as generator


class ReadHeavyGeneratorTest(unittest.TestCase):
    def config(self, root: Path, name: str) -> generator.GenerationConfig:
        return generator.GenerationConfig(
            output=root / name,
            stats=root / f"{name}.json",
            key_count=3,
            read_transactions=2,
            reads_per_transaction=2,
            sessions=2,
            key_bytes=8,
            value_bytes=9,
        )

    def test_exact_structure_counts_and_order(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = self.config(root, "trace.bin")
            stats = generator.generate(config)
            self.assertEqual(stats["transactions"], 3)
            self.assertEqual(stats["events"], 7)
            self.assertEqual(stats["read_events"], 4)
            self.assertEqual(stats["write_events"], 3)

            data = memoryview(config.output.read_bytes())
            offset = 0
            magic, version, flags, transactions, events = generator.TRACE_HEADER.unpack_from(data, offset)
            offset += generator.TRACE_HEADER.size
            self.assertEqual((magic, version, flags), (b"COTRACE1", 1, 0))
            self.assertEqual((transactions, events), (3, 7))

            observed = []
            for _ in range(transactions):
                sid, start_ts, commit_ts, op_count = generator.TXN_HEADER.unpack_from(data, offset)
                offset += generator.TXN_HEADER.size
                txn_ops = []
                for _ in range(op_count):
                    sequence, op, event_flags, reserved = generator.EVENT_HEADER.unpack_from(data, offset)
                    offset += generator.EVENT_HEADER.size
                    (key_size,) = generator.U32.unpack_from(data, offset)
                    offset += generator.U32.size
                    key = bytes(data[offset:offset + key_size])
                    offset += key_size
                    (value_size,) = generator.U32.unpack_from(data, offset)
                    offset += generator.U32.size
                    value = bytes(data[offset:offset + value_size])
                    offset += value_size
                    self.assertEqual((event_flags, reserved), (generator.HAS_VALUE, 0))
                    txn_ops.append((sequence, op, int.from_bytes(key[:8], "big"), int.from_bytes(value[:8], "big")))
                observed.append((sid, start_ts, commit_ts, txn_ops))

            self.assertEqual(offset, len(data))
            self.assertEqual([op[2] for op in observed[0][3]], [0, 1, 2])
            self.assertTrue(all(op[1] == generator.WRITE for op in observed[0][3]))
            self.assertEqual([op[2] for op in observed[1][3]], [0, 1])
            self.assertEqual([op[2] for op in observed[2][3]], [2, 0])
            self.assertTrue(all(op[1] == generator.READ for txn in observed[1:] for op in txn[3]))

            recorded = json.loads(config.stats.read_text(encoding="utf-8"))
            self.assertEqual(recorded["output_bytes"], config.output.stat().st_size)
            self.assertEqual(recorded["read_fraction"], 4 / 7)

    def test_output_is_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = self.config(root, "first.trace")
            second = dataclasses.replace(first, output=root / "second.trace", stats=None)
            generator.generate(first)
            generator.generate(second)
            first_hash = hashlib.sha256(first.output.read_bytes()).digest()
            second_hash = hashlib.sha256(second.output.read_bytes()).digest()
            self.assertEqual(first_hash, second_hash)

    def test_stale_history_counts_and_determinism(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = dataclasses.replace(
                self.config(root, "stale.trace"),
                history_rounds=2,
                repeated_writes=2,
            )
            stats = generator.generate(config)
            self.assertEqual(stats["transactions"], 7)
            self.assertEqual(stats["write_events"], 15)
            self.assertEqual(stats["history_write_events"], 12)
            self.assertEqual(stats["read_events"], 4)
            self.assertEqual(stats["events"], 19)
            self.assertEqual(stats["distinct_nonnull_values"], 6)
            self.assertEqual(stats["sessions"], 3)

    def test_rejects_invalid_ranges_and_aliases(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base = self.config(root, "bad.trace")
            with self.assertRaisesRegex(ValueError, "key-bytes"):
                generator.validate_config(dataclasses.replace(base, key_bytes=7))
            with self.assertRaisesRegex(ValueError, "must be different"):
                generator.validate_config(dataclasses.replace(base, stats=base.output))


if __name__ == "__main__":
    unittest.main()
