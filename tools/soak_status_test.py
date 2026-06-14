#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path

from soak_status import parse_state


class SoakStatusTest(unittest.TestCase):
    def test_counts_unique_remaining_routes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory) / "state.txt"
            state.write_text(
                "\n".join(
                    [
                        "VERSION 1",
                        'ACTIVE "0,1"',
                        'PENDING "0,1"',
                        'PENDING "0,2"',
                        'PENDING "0,2"',
                        'PENDING "0,3"',
                        'COMPLETED "0,3"',
                        'COMPLETED "0,0"',
                        'NODE "" choice "test.sdt:10" 2 "A\\nline" "" "B" ""',
                    ]
                ),
                encoding="utf-8",
            )

            summary = parse_state(state)

            self.assertEqual(summary.known_runs_remaining, 2)
            self.assertEqual(summary.active_runs, 1)
            self.assertEqual(summary.queued_runs, 1)
            self.assertEqual(summary.completed_runs, 2)
            self.assertEqual(summary.decision_nodes, 1)
            self.assertEqual(summary.duplicate_pending_records, 1)

    def test_rejects_unknown_version(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory) / "state.txt"
            state.write_text("VERSION 2\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "unsupported"):
                parse_state(state)


if __name__ == "__main__":
    unittest.main()
