#!/usr/bin/env python3
"""Summarize a ToHeart2 soak explorer state file."""

from __future__ import annotations

import argparse
import json
import shlex
import sys
from dataclasses import asdict, dataclass
from pathlib import Path


@dataclass(frozen=True)
class SoakSummary:
    known_runs_remaining: int
    active_runs: int
    queued_runs: int
    completed_runs: int
    decision_nodes: int
    duplicate_pending_records: int


def parse_state(path: Path) -> SoakSummary:
    active: set[str] = set()
    pending: set[str] = set()
    completed: set[str] = set()
    node_paths: set[str] = set()
    pending_records = 0
    version_seen = False

    try:
        source = path.read_text(encoding="utf-8")
    except OSError as error:
        raise ValueError(f"cannot read {path}: {error}") from error

    lexer = shlex.shlex(source, posix=True)
    lexer.whitespace_split = True
    lexer.commenters = ""
    try:
        tokens = list(lexer)
    except ValueError as error:
        raise ValueError(f"{path}: {error}") from error

    position = 0

    def take(record: str) -> str:
        nonlocal position
        if position >= len(tokens):
            raise ValueError(f"{path}: truncated {record} record")
        value = tokens[position]
        position += 1
        return value

    while position < len(tokens):
        record = take("state")
        if record == "VERSION":
            version = take(record)
            if version_seen or version != "1":
                raise ValueError(
                    f"{path}: unsupported or duplicate version"
                )
            version_seen = True
        elif record in {"ACTIVE", "PENDING", "COMPLETED"}:
            route = take(record)
            if record == "ACTIVE":
                active.add(route)
            elif record == "PENDING":
                pending_records += 1
                pending.add(route)
            else:
                completed.add(route)
        elif record == "NODE":
            node_paths.add(take(record))
            take(record)  # decision kind
            take(record)  # checkpoint
            raw_count = take(record)
            try:
                option_count = int(raw_count)
            except ValueError as error:
                raise ValueError(
                    f"{path}: invalid NODE option count {raw_count!r}"
                ) from error
            if option_count < 0:
                raise ValueError(f"{path}: negative NODE option count")
            for _ in range(option_count):
                take(record)  # label
                take(record)  # target
        else:
            raise ValueError(f"{path}: unknown record type {record!r}")

    if not version_seen:
        raise ValueError(f"{path}: missing VERSION record")
    if len(active) > 1:
        raise ValueError(f"{path}: contains more than one active route")

    remaining = (active | pending) - completed
    return SoakSummary(
        known_runs_remaining=len(remaining),
        active_runs=len(active - completed),
        queued_runs=len(pending - completed - active),
        completed_runs=len(completed),
        decision_nodes=len(node_paths),
        duplicate_pending_records=pending_records - len(pending),
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
        summary = parse_state(arguments.state)
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
