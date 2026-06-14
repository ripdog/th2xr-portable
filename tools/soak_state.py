"""Read, write, and merge ToHeart2 soak explorer state files."""

from __future__ import annotations

import shlex
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path


@dataclass(frozen=True)
class SoakOption:
    label: str
    target: str


@dataclass(frozen=True)
class SoakNode:
    kind: str
    checkpoint: str
    options: tuple[SoakOption, ...]


@dataclass
class SoakState:
    active: list[str] = field(default_factory=list)
    pending: list[str] = field(default_factory=list)
    completed: list[str] = field(default_factory=list)
    nodes: dict[str, SoakNode] = field(default_factory=dict)
    pending_records: int = 0

    @property
    def remaining(self) -> list[str]:
        completed = set(self.completed)
        return unique(
            route
            for route in [*self.active, *self.pending]
            if route not in completed
        )


def unique(routes: Iterable[str]) -> list[str]:
    return list(dict.fromkeys(routes))


def append_unique(routes: list[str], route: str) -> None:
    if route not in routes:
        routes.append(route)


def remove_all(routes: list[str], removed: Iterable[str]) -> None:
    removed_set = set(removed)
    routes[:] = [route for route in routes if route not in removed_set]


def is_subtree_path(route: str, prefix: str) -> bool:
    return not prefix or route == prefix or route.startswith(prefix + ",")


def prune_subtree(state: SoakState, prefix: str) -> tuple[int, int]:
    removed_routes = unique(
        route
        for route in [*state.active, *state.pending, *state.completed]
        if is_subtree_path(route, prefix)
    )
    state.active = [
        route for route in state.active
        if not is_subtree_path(route, prefix)
    ]
    state.pending = [
        route for route in state.pending
        if not is_subtree_path(route, prefix)
    ]
    state.completed = [
        route for route in state.completed
        if not is_subtree_path(route, prefix)
    ]

    removed_nodes = [
        route for route in state.nodes if is_subtree_path(route, prefix)
    ]
    for route in removed_nodes:
        del state.nodes[route]

    state.pending.insert(0, prefix)
    state.pending_records = len(state.pending)
    return len(removed_routes), len(removed_nodes)


def quoted(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def parse_state(path: Path) -> SoakState:
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

    state = SoakState()
    position = 0
    version_seen = False

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
                append_unique(state.active, route)
            elif record == "PENDING":
                state.pending_records += 1
                append_unique(state.pending, route)
            else:
                append_unique(state.completed, route)
        elif record == "NODE":
            route = take(record)
            kind = take(record)
            checkpoint = take(record)
            raw_count = take(record)
            try:
                option_count = int(raw_count)
            except ValueError as error:
                raise ValueError(
                    f"{path}: invalid NODE option count {raw_count!r}"
                ) from error
            if option_count < 0:
                raise ValueError(f"{path}: negative NODE option count")
            options = tuple(
                SoakOption(take(record), take(record))
                for _ in range(option_count)
            )
            node = SoakNode(kind, checkpoint, options)
            previous = state.nodes.get(route)
            if previous is not None and previous != node:
                raise ValueError(f"{path}: conflicting NODE for {route!r}")
            state.nodes[route] = node
        else:
            raise ValueError(f"{path}: unknown record type {record!r}")

    if not version_seen:
        raise ValueError(f"{path}: missing VERSION record")
    return state


def write_state(path: Path, state: SoakState) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as output:
        output.write("VERSION 1\n")
        for route in state.active:
            output.write(f"ACTIVE {quoted(route)}\n")
        for route in state.pending:
            output.write(f"PENDING {quoted(route)}\n")
        for route in state.completed:
            output.write(f"COMPLETED {quoted(route)}\n")
        for route, node in state.nodes.items():
            output.write(
                f"NODE {quoted(route)} {node.kind} "
                f"{quoted(node.checkpoint)} {len(node.options)}"
            )
            for option in node.options:
                output.write(
                    f" {quoted(option.label)} {quoted(option.target)}"
                )
            output.write("\n")
    temporary.replace(path)


def merge_state(destination: SoakState, source: SoakState) -> None:
    destination.pending = unique(
        [*destination.pending, *source.pending, *source.active]
    )
    destination.completed = unique(
        [*destination.completed, *source.completed]
    )
    for route, node in source.nodes.items():
        previous = destination.nodes.get(route)
        if previous is not None and previous != node:
            raise ValueError(
                f"conflicting decision node at path {route or '<root>'}: "
                f"{previous!r} != {node!r}"
            )
        destination.nodes[route] = node
    remove_all(destination.pending, destination.completed)
    remove_all(destination.active, destination.completed)
    destination.pending_records = len(destination.pending)
