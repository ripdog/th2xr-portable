#!/usr/bin/env python3
"""Prune an invalid decision subtree from a soak campaign."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from soak_state import is_subtree_path, parse_state, prune_subtree, write_state


def independent_prefixes(paths: list[str]) -> list[str]:
    result: list[str] = []
    for path in paths:
        if any(is_subtree_path(path, prefix) for prefix in result):
            continue
        result = [
            prefix for prefix in result if not is_subtree_path(prefix, path)
        ]
        result.append(path)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Remove a decision node and all routes below it, then queue the "
            "decision prefix for rediscovery."
        )
    )
    parser.add_argument(
        "path",
        nargs="?",
        help="comma-separated decision path to prune",
    )
    parser.add_argument(
        "--checkpoint",
        help="prune every decision node at this checkpoint",
    )
    parser.add_argument(
        "--first-label",
        help="with --checkpoint, only prune nodes whose first option label matches",
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
        if arguments.path and arguments.checkpoint:
            parser.error("pass either a path or --checkpoint, not both")
        if not arguments.path and not arguments.checkpoint:
            parser.error("pass a path or --checkpoint")
        if arguments.first_label and not arguments.checkpoint:
            parser.error("--first-label requires --checkpoint")

        state = parse_state(state_path)
        if arguments.checkpoint:
            paths = [
                path for path, node in state.nodes.items()
                if node.checkpoint == arguments.checkpoint
                and (
                    arguments.first_label is None
                    or (
                        len(node.options) != 0
                        and node.options[0].label == arguments.first_label
                    )
                )
            ]
            if not paths:
                parser.error("no matching decision nodes found")
            paths = independent_prefixes(paths)
        else:
            if arguments.path not in state.nodes:
                parser.error(
                    f"no decision node exists at path {arguments.path!r}"
                )
            paths = [arguments.path]

        total_routes = 0
        total_nodes = 0
        for path in paths:
            removed_routes, removed_nodes = prune_subtree(state, path)
            total_routes += removed_routes
            total_nodes += removed_nodes
            print(
                f"prune {path}: remove {removed_nodes} decision nodes "
                f"and {removed_routes} routes; queue prefix for rediscovery"
            )
        print(
            f"total: remove {total_nodes} decision nodes and "
            f"{total_routes} routes across {len(paths)} prefixes"
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
