#!/usr/bin/env python3
"""Focused argument-validation tests for generate_ycsb.sh."""

from __future__ import annotations

import os
import subprocess
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("generate_ycsb.sh")
CONFIG_ENV = {
    "FAIL_ON_RUN_ERRORS",
    "BINARY_ONLY",
    "LOAD_BATCH_SIZE",
    "LOAD_THREADS",
    "OPS_PER_TXN",
    "RECORD_COUNT",
    "RETRY_CONFLICTED_TXNS",
    "SESSIONS",
    "TARGET",
    "THETA",
    "TXN_COUNT",
}


class GenerateYcsbScriptTest(unittest.TestCase):
    def run_script(self, **overrides: str) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        for name in CONFIG_ENV:
            env.pop(name, None)
        env.update(overrides)
        return subprocess.run(
            ["bash", str(SCRIPT)],
            check=False,
            capture_output=True,
            text=True,
            env=env,
        )

    def test_help_describes_the_three_primary_knobs(self) -> None:
        result = subprocess.run(
            ["bash", str(SCRIPT), "--help"],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("THETA=0.99", result.stdout)
        self.assertIn("OPS_PER_TXN=1", result.stdout)
        self.assertIn("SESSIONS=1", result.stdout)
        self.assertIn("RETRY_CONFLICTED_TXNS=1", result.stdout)

    def test_theta_must_be_strictly_between_zero_and_one(self) -> None:
        for theta in ("1", "garbage"):
            with self.subTest(theta=theta):
                result = self.run_script(THETA=theta)

                self.assertEqual(2, result.returncode)
                self.assertIn("THETA must be", result.stderr)

    def test_every_requested_session_must_receive_a_transaction(self) -> None:
        result = self.run_script(TXN_COUNT="2", SESSIONS="3")

        self.assertEqual(2, result.returncode)
        self.assertIn("must be at least SESSIONS", result.stderr)

    def test_load_batches_must_not_overshoot_record_count(self) -> None:
        result = self.run_script(
            RECORD_COUNT="100", LOAD_THREADS="3", LOAD_BATCH_SIZE="10"
        )

        self.assertEqual(2, result.returncode)
        self.assertIn("must be divisible", result.stderr)

    def test_fail_on_run_errors_is_boolean(self) -> None:
        result = self.run_script(FAIL_ON_RUN_ERRORS="yes")

        self.assertEqual(2, result.returncode)
        self.assertIn("must be 0 or 1", result.stderr)

    def test_binary_only_is_boolean(self) -> None:
        result = self.run_script(BINARY_ONLY="yes")

        self.assertEqual(2, result.returncode)
        self.assertIn("BINARY_ONLY must be 0 or 1", result.stderr)

    def test_retry_conflicted_transactions_is_boolean(self) -> None:
        result = self.run_script(RETRY_CONFLICTED_TXNS="yes")

        self.assertEqual(2, result.returncode)
        self.assertIn("RETRY_CONFLICTED_TXNS must be 0 or 1", result.stderr)

    def test_port_must_fit_tcp_range(self) -> None:
        result = self.run_script(PORT="65536")

        self.assertEqual(2, result.returncode)
        self.assertIn("must not exceed 65535", result.stderr)


if __name__ == "__main__":
    unittest.main()
