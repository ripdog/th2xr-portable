#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path

from soak_parallel import (
    lease_routes,
    merge_worker,
    requeue_failed_routes,
    worker_state,
)
from soak_state import (
    SoakNode,
    SoakOption,
    SoakState,
    merge_state,
    parse_state,
    prune_subtree,
    write_state,
)


class SoakStateTest(unittest.TestCase):
    def test_round_trip_preserves_multiline_options(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.txt"
            source = SoakState(
                active=["0"],
                pending=["1"],
                completed=["2"],
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
            self.assertEqual(loaded.active, ["0"])
            self.assertEqual(loaded.pending, ["1"])
            self.assertEqual(loaded.completed, ["2"])
            self.assertEqual(loaded.nodes, source.nodes)

    def test_merge_adds_discovered_siblings(self) -> None:
        destination = SoakState(
            active=["0"],
            pending=["1"],
            completed=["2"],
            nodes={},
        )
        source = SoakState(
            pending=["0,1"],
            completed=["0"],
            nodes={
                "0": SoakNode(
                    "map", "map.sdt:20", (SoakOption("A", "script"),)
                )
            },
        )
        merge_state(destination, source)
        self.assertEqual(destination.pending, ["1", "0,1"])
        self.assertEqual(destination.completed, ["2", "0"])
        self.assertIn("0", destination.nodes)

    def test_missing_worker_state_requeues_lease(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            state = SoakState()
            merge_worker(state, Path(directory), "1,2")
            self.assertEqual(state.pending, ["1,2"])

    def test_worker_receives_only_its_lease(self) -> None:
        global_state = SoakState(
            pending=["0", "1"],
            completed=["2"],
            nodes={
                "": SoakNode(
                    "choice", "test.sdt:10", (SoakOption("A", ""),)
                )
            },
        )
        state = worker_state(global_state, "1")
        self.assertEqual(state.pending, ["1"])
        self.assertEqual(state.completed, ["2"])
        self.assertEqual(state.nodes, global_state.nodes)

    def test_pending_queue_order_survives_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.txt"
            expected = ["0,0,0,1", "2", "0,1"]
            write_state(path, SoakState(pending=expected))
            self.assertEqual(parse_state(path).pending, expected)

    def test_leases_front_of_existing_queue(self) -> None:
        state = SoakState(
            active=["interrupted"],
            pending=["deep-live-path", "0", "0,0"],
            completed=["already-done"],
        )
        leased = lease_routes(state, 2)
        self.assertEqual(leased, ["interrupted", "deep-live-path"])
        self.assertEqual(state.active, leased)
        self.assertEqual(state.pending, ["0", "0,0"])

    def test_prune_subtree_invalidates_descendants(self) -> None:
        state = SoakState(
            active=["1,0,2"],
            pending=["0", "1,0,1", "2"],
            completed=["1,0,0", "3"],
            nodes={
                "": SoakNode("choice", "root", ()),
                "1,0": SoakNode("map", "stale", ()),
                "1,0,0": SoakNode("choice", "descendant", ()),
                "2": SoakNode("choice", "unrelated", ()),
            },
        )

        removed_routes, removed_nodes = prune_subtree(state, "1,0")

        self.assertEqual((removed_routes, removed_nodes), (3, 2))
        self.assertEqual(state.active, [])
        self.assertEqual(state.pending, ["1,0", "0", "2"])
        self.assertEqual(state.completed, ["3"])
        self.assertEqual(list(state.nodes), ["", "2"])

    def test_failed_routes_return_to_front_in_lease_order(self) -> None:
        state = SoakState(pending=["later", "failed-1"])
        requeue_failed_routes(state, ["failed-1", "failed-2"])
        self.assertEqual(
            state.pending, ["failed-1", "failed-2", "later"]
        )


if __name__ == "__main__":
    unittest.main()
