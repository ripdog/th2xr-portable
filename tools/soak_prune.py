#!/usr/bin/env python3
"""Prune an invalid decision subtree from a soak campaign."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from soak_state import parse_state, prune_subtree, write_state


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Remove a decision node and all routes below it, then queue the "
            "decision prefix for rediscovery."
        )
    )
    parser.add_argument(
        "path",
        help="comma-separated decision path to prune",
    )
    parser.add_argument(
        "--state",
        type=Path,
        default=Path("logs/soak"),
        help="campaign directory (default: logs/soak)",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="write the repaired state; otherwise only preview changes",
    )
    arguments = parser.parse_args()

    state_path = arguments.state / "state.txt"
    lock_path = arguments.state / "parallel.lock"
    if lock_path.exists():
        parser.error(
            f"parallel coordinator is active ({lock_path}); stop it first"
        )

    try:
        state = parse_state(state_path)
        if arguments.path not in state.nodes:
            parser.error(
                f"no decision node exists at path {arguments.path!r}"
            )
        removed_routes, removed_nodes = prune_subtree(
            state, arguments.path
        )
        print(
            f"prune {arguments.path}: remove {removed_nodes} decision nodes "
            f"and {removed_routes} routes; queue prefix for rediscovery"
        )
        if arguments.apply:
            write_state(state_path, state)
            print(f"updated {state_path}")
        else:
            print("preview only; pass --apply to update the state")
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
