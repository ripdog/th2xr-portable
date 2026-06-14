#!/usr/bin/env python3
"""Summarize a ToHeart2 soak explorer state file."""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

from soak_state import parse_state


@dataclass(frozen=True)
class SoakSummary:
    known_runs_remaining: int
    active_runs: int
    queued_runs: int
    completed_runs: int
    decision_nodes: int
    duplicate_pending_records: int


def summarize_state(path: Path) -> SoakSummary:
    state = parse_state(path)
    return SoakSummary(
        known_runs_remaining=len(state.remaining),
        active_runs=len(state.active - state.completed),
        queued_runs=len(state.pending - state.completed - state.active),
        completed_runs=len(state.completed),
        decision_nodes=len(state.nodes),
        duplicate_pending_records=state.pending_records - len(state.pending),
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Calculate known full-game runs remaining in soak state."
    )
    parser.add_argument(
        "state",
        nargs="?",
        type=Path,
        default=Path("logs/soak/state.txt"),
        help="state file (default: logs/soak/state.txt)",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit a machine-readable JSON object",
    )
    arguments = parser.parse_args()

    try:
        summary = summarize_state(arguments.state)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1

    if arguments.json:
        print(json.dumps(asdict(summary), sort_keys=True))
        return 0

    print(f"Known game runs left: {summary.known_runs_remaining}")
    print(f"  active/incomplete: {summary.active_runs}")
    print(f"  queued:            {summary.queued_runs}")
    print(f"Completed runs:       {summary.completed_runs}")
    print(f"Decision nodes:       {summary.decision_nodes}")
    if summary.duplicate_pending_records:
        print(
            "Duplicate pending records ignored: "
            f"{summary.duplicate_pending_records}"
        )
    print(
        "Note: this is the current known frontier; later runs can discover "
        "additional branches."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
