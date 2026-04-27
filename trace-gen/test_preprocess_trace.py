from __future__ import annotations

import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path

import yaml


MODULE_PATH = Path(__file__).with_name("preprocess_trace.py")
SPEC = importlib.util.spec_from_file_location("preprocess_trace", MODULE_PATH)
assert SPEC is not None
preprocess_trace = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = preprocess_trace
SPEC.loader.exec_module(preprocess_trace)


class PreprocessTraceTest(unittest.TestCase):
    def test_keeps_committed_transactions_in_commit_order(self) -> None:
        documents = [
            {
                "session_id": 1,
                "status": "committed",
                "start_ts": 100,
                "commit_ts": 300,
                "events": [{"seq": 1, "op": "W", "key_hex": "6b31"}],
            },
            {
                "session_id": 2,
                "status": "rolled_back",
                "start_ts": 200,
                "events": [{"seq": 1, "op": "W", "key_hex": "6b32"}],
            },
            {
                "session_id": 3,
                "status": "committed",
                "start_ts": 101,
                "commit_ts": 100,
                "events": [],
            },
            {
                "session_id": 4,
                "start_ts": 102,
                "commit_ts": 200,
                "events": [],
            },
            {
                "status": "committed",
                "start_ts": 103,
                "commit_ts": 50,
                "events": [],
            },
        ]

        result = preprocess_trace.preprocess_documents(documents)

        self.assertEqual([doc["commit_ts"] for doc in result], [100, 200, 300])
        self.assertTrue(all("status" not in doc for doc in result))
        self.assertTrue(all("session_id" in doc for doc in result))

    def test_rejects_committed_transaction_without_commit_ts(self) -> None:
        with self.assertRaises(preprocess_trace.TracePreprocessError):
            preprocess_trace.preprocess_documents(
                [{"status": "committed", "start_ts": 100, "events": []}]
            )

    def test_write_trace_outputs_parseable_yaml_stream(self) -> None:
        documents = [
            {"session_id": 1, "start_ts": 1, "commit_ts": 2, "events": []}
        ]
        output = io.StringIO()

        preprocess_trace.write_trace(documents, output)

        parsed = list(yaml.safe_load_all(output.getvalue()))
        self.assertEqual(parsed, documents)

    def test_write_trace_indents_event_list_items(self) -> None:
        documents = [
            {
                "session_id": 1,
                "start_ts": 1,
                "commit_ts": 2,
                "events": [{"seq": 1, "op": "R"}],
            }
        ]
        output = io.StringIO()

        preprocess_trace.write_trace(documents, output)

        self.assertIn("events:\n  - seq: 1\n    op: R\n", output.getvalue())

    def test_normalizes_session_ids_per_trace(self) -> None:
        documents = [
            {
                "session_id": 8,
                "status": "committed",
                "start_ts": 100,
                "commit_ts": 300,
                "events": [],
            },
            {
                "session_id": 7,
                "status": "committed",
                "start_ts": 101,
                "commit_ts": 200,
                "events": [],
            },
            {
                "session_id": 8,
                "status": "committed",
                "start_ts": 102,
                "commit_ts": 400,
                "events": [],
            },
        ]

        result = preprocess_trace.preprocess_documents(documents)

        self.assertEqual([doc["session_id"] for doc in result], [1, 2, 2])

    def test_filters_mem_buffer_reads_and_drops_source_metadata(self) -> None:
        documents = [
            {
                "session_id": 1,
                "status": "committed",
                "start_ts": 1,
                "commit_ts": 2,
                "events": [
                    {"seq": 1, "op": "W", "key_hex": "6b31"},
                    {
                        "seq": 2,
                        "op": "R",
                        "key_hex": "6b31",
                        "miss": True,
                        "source": "mem_buffer",
                    },
                    {
                        "seq": 3,
                        "op": "R",
                        "key_hex": "6b31",
                        "value_hex": "7631",
                        "source": "snapshot",
                    },
                    {
                        "seq": 4,
                        "op": "W",
                        "key_hex": "6b32",
                        "source": "mem_buffer",
                    },
                ],
            }
        ]

        result = preprocess_trace.preprocess_documents(documents)

        self.assertEqual(
            result[0]["events"],
            [
                {"seq": 1, "op": "W", "key_hex": "6b31"},
                {"seq": 3, "op": "R", "key_hex": "6b31", "value_hex": "7631"},
                {"seq": 4, "op": "W", "key_hex": "6b32"},
            ],
        )

    def test_in_place_preprocess_preserves_file_mode(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "trace.yaml"
            path.write_text(
                """\
---
session_id: 10
status: committed
start_ts: 1
commit_ts: 2
events: []
---
status: rolled_back
start_ts: 3
events: []
""",
                encoding="utf-8",
            )
            path.chmod(0o644)

            preprocess_trace.preprocess_file(path, None, True)

            self.assertEqual(path.stat().st_mode & 0o777, 0o644)
            docs = list(yaml.safe_load_all(path.read_text(encoding="utf-8")))
            self.assertEqual(len(docs), 1)
            self.assertEqual(docs[0]["commit_ts"], 2)

    def test_rename_uses_preprocessed_trace_counts(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "3s-large-mixed.yaml"
            path.write_text(
                """\
---
session_id: 11
status: committed
start_ts: 1
commit_ts: 2
events:
  - seq: 1
    op: R
  - seq: 2
    op: W
---
session_id: 12
status: rolled_back
start_ts: 3
events:
  - seq: 1
    op: W
---
session_id: 12
status: committed
start_ts: 4
commit_ts: 5
events:
  - seq: 1
    op: R
""",
                encoding="utf-8",
            )

            output_path = preprocess_trace.preprocess_file(path, None, True, True)

            self.assertEqual(output_path.name, "2s-2t-3e-large-mixed.yaml")
            self.assertFalse(path.exists())
            self.assertTrue(output_path.exists())


if __name__ == "__main__":
    unittest.main()
