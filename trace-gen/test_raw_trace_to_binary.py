from __future__ import annotations

import importlib.util
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("raw_trace_to_binary.py")
SPEC = importlib.util.spec_from_file_location("raw_trace_to_binary", MODULE_PATH)
assert SPEC is not None
converter = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = converter
SPEC.loader.exec_module(converter)

CO_CHECKER_ROOT = MODULE_PATH.parent.parent
TRACE_PREPROCESS = CO_CHECKER_ROOT / "build" / "trace-preprocess"
TRACE_CONVERT = CO_CHECKER_ROOT / "build" / "trace-convert"


RAW_TRACE = b"""\
---
session_id: 90
status: "committed"
start_ts: 3
commit_ts: 30
events:
  - seq: 1
    op: "W"
    key_hex: "aa"
    value_hex: "bbcc"
    source: "mem_buffer"
---
session_id: 70
status: "rolled_back"
start_ts: 4
events:
  - seq: 1
    op: "W"
    key_hex: "ff"
---
status: "committed"
start_ts: 1
commit_ts: 5
events: []
---
session_id: 80
start_ts: 2
commit_ts: 10
events:
  - seq: 1
    op: "R"
    key_hex: "01"
    value_hex: "02"
    source: "snapshot"
  - seq: 2
    op: "R"
    key_hex: "03"
    miss: true
    source: "mem_buffer"
  - seq: 3
    op: "W"
    key_hex: "04"
    delete: true
---
session_id: 90
status: "committed"
start_ts: 6
commit_ts: 20
events:
  - seq: 7
    op: "R"
    key_hex: "05"
    miss: true
    source: "snapshot"
"""


def byte_string(value: bytes) -> bytes:
    return struct.pack("<I", len(value)) + value


def event(
    seq: int,
    op: int,
    flags: int,
    key: bytes,
    value: bytes | None = None,
) -> bytes:
    result = struct.pack("<IBBH", seq, op, flags, 0) + byte_string(key)
    if value is not None:
        result += byte_string(value)
    return result


EXPECTED_TRACE = b"".join(
    (
        struct.pack("<8sIIQQ", b"COTRACE1", 1, 0, 3, 4),
        struct.pack("<IQQI", 1, 2, 10, 2),
        event(1, 0, 1, b"\x01", b"\x02"),
        event(3, 1, 2, b"\x04"),
        struct.pack("<IQQI", 2, 6, 20, 1),
        event(7, 0, 4, b"\x05"),
        struct.pack("<IQQI", 2, 3, 30, 1),
        event(1, 1, 1, b"\xaa", b"\xbb\xcc"),
    )
)


class RawTraceToBinaryTest(unittest.TestCase):
    def test_filters_sorts_densifies_and_writes_exact_cotrace1(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            raw = root / "input.raw.yaml"
            raw.write_bytes(RAW_TRACE)

            result = converter.convert_trace(raw, root / "experiment.trace", True)

            self.assertEqual(result.name, "2s-3t-4e-experiment.trace")
            self.assertEqual(result.read_bytes(), EXPECTED_TRACE)

    def test_metadata_index_does_not_retain_documents_or_events(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            raw = Path(temp_dir) / "input.raw.yaml"
            raw.write_bytes(RAW_TRACE)

            transactions, stats = converter.scan_trace(raw)

            self.assertEqual(stats, converter.TraceStats(2, 3, 4))
            self.assertEqual([txn.commit_ts for txn in transactions], [10, 20, 30])
            for transaction in transactions:
                self.assertEqual(
                    set(transaction.__dict__),
                    {
                        "original_index",
                        "offset",
                        "length",
                        "session_id",
                        "start_ts",
                        "commit_ts",
                        "event_count",
                    },
                )

    def test_rejects_duplicate_commit_timestamps_before_writing(self) -> None:
        raw_trace = b"""\
---
session_id: 1
status: committed
start_ts: 1
commit_ts: 3
events: []
---
session_id: 2
status: committed
start_ts: 2
commit_ts: 3
events: []
"""
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            raw = root / "input.raw.yaml"
            output = root / "output.trace"
            raw.write_bytes(raw_trace)

            with self.assertRaisesRegex(
                converter.RawTraceConversionError, "duplicates"
            ):
                converter.convert_trace(raw, output)

            self.assertFalse(output.exists())

    def test_rejects_non_tidb_stream_without_document_markers(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            raw = Path(temp_dir) / "input.raw.yaml"
            raw.write_text("session_id: 1\n", encoding="utf-8")

            with self.assertRaisesRegex(
                converter.RawTraceConversionError, "no `---` marker"
            ):
                converter.scan_trace(raw)

    @unittest.skipUnless(
        TRACE_PREPROCESS.is_file() and TRACE_CONVERT.is_file(),
        "co-checker conversion tools are not built",
    )
    def test_output_is_byte_identical_to_existing_cpp_pipeline(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            raw = root / "input.raw.yaml"
            normalized = root / "normalized.yaml"
            expected = root / "expected.trace"
            actual = root / "actual.trace"
            raw.write_bytes(RAW_TRACE)

            subprocess.run(
                [str(TRACE_PREPROCESS), "-o", str(normalized), str(raw)],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run(
                [str(TRACE_CONVERT), "-o", str(expected), str(normalized)],
                check=True,
                capture_output=True,
                text=True,
            )
            converter.convert_trace(raw, actual)

            self.assertEqual(actual.read_bytes(), expected.read_bytes())


if __name__ == "__main__":
    unittest.main()
