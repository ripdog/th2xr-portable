"""Read, write, and merge ToHeart2 soak explorer state files."""

from __future__ import annotations

import shlex
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
    active: set[str] = field(default_factory=set)
    pending: set[str] = field(default_factory=set)
    completed: set[str] = field(default_factory=set)
    nodes: dict[str, SoakNode] = field(default_factory=dict)
    pending_records: int = 0

    @property
    def remaining(self) -> set[str]:
        return (self.active | self.pending) - self.completed


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
                state.active.add(route)
            elif record == "PENDING":
                state.pending_records += 1
                state.pending.add(route)
            else:
                state.completed.add(route)
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
        for route in sorted(state.active, key=route_key):
            output.write(f"ACTIVE {quoted(route)}\n")
        for route in sorted(state.pending, key=route_key):
            output.write(f"PENDING {quoted(route)}\n")
        for route in sorted(state.completed, key=route_key):
            output.write(f"COMPLETED {quoted(route)}\n")
        for route in sorted(state.nodes, key=route_key):
            node = state.nodes[route]
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
    destination.pending.update(source.pending)
    destination.pending.update(source.active)
    destination.completed.update(source.completed)
    for route, node in source.nodes.items():
        previous = destination.nodes.get(route)
        if previous is not None and previous != node:
            raise ValueError(
                f"conflicting decision node at path {route or '<root>'}: "
                f"{previous!r} != {node!r}"
            )
        destination.nodes[route] = node
    destination.pending.difference_update(destination.completed)
    destination.active.difference_update(destination.completed)
    destination.pending_records = len(destination.pending)


def route_key(route: str) -> tuple[int, tuple[int, ...]]:
    if not route:
        return (0, ())
    try:
        values = tuple(int(value) for value in route.split(","))
    except ValueError:
        return (2, tuple(ord(value) for value in route))
    return (1, values)
