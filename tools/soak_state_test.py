#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path

from soak_parallel import merge_worker, worker_state
from soak_state import (
    SoakNode,
    SoakOption,
    SoakState,
    merge_state,
    parse_state,
    write_state,
)


class SoakStateTest(unittest.TestCase):
    def test_round_trip_preserves_multiline_options(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.txt"
            source = SoakState(
                active={"0"},
                pending={"1"},
                completed={"2"},
                nodes={
                    "": SoakNode(
                        "choice",
                        "test.sdt:10",
                        (SoakOption('a "quoted"\nline', "target"),),
                    )
                },
            )
            write_state(path, source)
            loaded = parse_state(path)
            self.assertEqual(loaded.active, source.active)
            self.assertEqual(loaded.pending, source.pending)
            self.assertEqual(loaded.completed, source.completed)
            self.assertEqual(loaded.nodes, source.nodes)

    def test_merge_adds_discovered_siblings(self) -> None:
        destination = SoakState(
            active={"0"},
            pending={"1"},
            completed={"2"},
            nodes={},
        )
        source = SoakState(
            pending={"0,1"},
            completed={"0"},
            nodes={
                "0": SoakNode(
                    "map", "map.sdt:20", (SoakOption("A", "script"),)
                )
            },
        )
        merge_state(destination, source)
        self.assertEqual(destination.pending, {"0,1", "1"})
        self.assertEqual(destination.completed, {"0", "2"})
        self.assertIn("0", destination.nodes)

    def test_missing_worker_state_requeues_lease(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            state = SoakState()
            merge_worker(state, Path(directory), "1,2")
            self.assertEqual(state.pending, {"1,2"})

    def test_worker_receives_only_its_lease(self) -> None:
        global_state = SoakState(
            pending={"0", "1"},
            completed={"2"},
            nodes={
                "": SoakNode(
                    "choice", "test.sdt:10", (SoakOption("A", ""),)
                )
            },
        )
        state = worker_state(global_state, "1")
        self.assertEqual(state.pending, {"1"})
        self.assertEqual(state.completed, {"2"})
        self.assertEqual(state.nodes, global_state.nodes)


if __name__ == "__main__":
    unittest.main()
