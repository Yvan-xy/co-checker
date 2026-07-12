import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "tools" / "sample_trace_layout.py"
SPEC = importlib.util.spec_from_file_location("sample_trace_layout", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
sampler = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = sampler
SPEC.loader.exec_module(sampler)


def blob(value: bytes) -> bytes:
    return struct.pack("<I", len(value)) + value


def event(sequence: int, operation: int, flags: int, key: bytes, value=b""):
    encoded = struct.pack("<IBBH", sequence, operation, flags, 0) + blob(key)
    if flags & sampler.HAS_VALUE:
        encoded += blob(value)
    return encoded


class LayoutSamplerTest(unittest.TestCase):
    def write_trace(self, path: Path, events: list[bytes]) -> None:
        header = sampler.TRACE_MAGIC + struct.pack("<IIQQ", 1, 0, 1, len(events))
        transaction = struct.pack("<IQQI", 7, 10, 20, len(events))
        path.write_bytes(header + transaction + b"".join(events))

    def test_streaming_sample_preserves_empty_and_absent_tags(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.trace"
            events = [
                event(1, 0, sampler.MISS, b"a"),
                event(2, 0, sampler.HAS_VALUE, b"b", b""),
                event(3, 1, sampler.HAS_VALUE, b"c", b"v"),
                event(4, 1, sampler.DELETE, b"d"),
            ]
            self.write_trace(path, events)
            report = sampler.sample(path, [32, 64])

            self.assertEqual(report["transactions"]["events"]["total"], 4)
            self.assertEqual(report["transactions"]["reads"]["total"], 2)
            self.assertEqual(report["transactions"]["writes"]["total"], 2)
            self.assertEqual(report["transactions"]["payload_bytes"]["total"], 5)
            self.assertEqual(report["transactions"]["sealed_used_bytes"]["total"], 301)
            self.assertEqual(len(report["chunk_policies"]), 2)
            self.assertGreaterEqual(
                report["chunk_policies"][0]["reserved_bytes"], 301
            )

    def test_rejects_missing_required_tag(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "invalid.trace"
            self.write_trace(path, [event(1, 0, 0, b"a")])
            with self.assertRaises(sampler.TraceFormatError):
                sampler.sample(path, [64])

    def test_dedicated_oversized_blob_is_exactly_reserved(self):
        shape = sampler.TransactionShape(
            event_count=1,
            read_count=1,
            payload_bytes=101,
            events=[sampler.EventShape(1, 100, True, True)],
        )
        layout = sampler.layout_for_chunk_size(shape, 64)
        self.assertEqual(layout.descriptor_segments, 1)
        self.assertEqual(layout.payload_segments, 2)
        self.assertEqual(layout.dedicated_segments, 1)
        self.assertEqual(layout.used_bytes, 301)
        self.assertEqual(layout.reserved_bytes, 396)


if __name__ == "__main__":
    unittest.main()
