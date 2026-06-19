#!/usr/bin/env python3
"""Move legacy save-type state from toheart2-config.ini into SQLite."""

from __future__ import annotations

import argparse
import re
import shutil
import sqlite3
import sys
from dataclasses import dataclass
from pathlib import Path


GAME_FLAG_RE = re.compile(r"^game_flag_(\d+)$")


@dataclass
class MigratedState:
    read_lines: set[tuple[str, int, int]]
    game_flags: dict[int, int]
    unlocks: set[tuple[str, int]]
    cleaned_lines: list[str]
    skipped_reads: int = 0
    skipped_flags: int = 0
    skipped_unlocks: int = 0


def parse_int(value: str) -> int | None:
    try:
        return int(value)
    except ValueError:
        return None


def parse_read_key(value: str) -> tuple[str, int, int] | None:
    script, separator, rest = value.partition(":")
    if not separator:
        return None
    position, separator, revealed = rest.rpartition(":")
    if not separator:
        return None
    parsed_position = parse_int(position)
    parsed_revealed = parse_int(revealed)
    if parsed_position is None or parsed_revealed is None:
        return None
    return script, parsed_position, parsed_revealed


def parse_ini(path: Path) -> MigratedState:
    state = MigratedState(set(), {}, set(), [])
    for raw_line in path.read_text(encoding="utf-8").splitlines(keepends=True):
        line = raw_line.rstrip("\r\n")
        key, separator, value = line.partition("=")
        if not separator:
            state.cleaned_lines.append(raw_line)
            continue

        if key == "read":
            marker = parse_read_key(value)
            if marker is None:
                state.skipped_reads += 1
            else:
                state.read_lines.add(marker)
            continue

        if key.startswith("name_"):
            continue

        game_flag = GAME_FLAG_RE.match(key)
        if game_flag:
            index = parse_int(game_flag.group(1))
            flag_value = parse_int(value)
            if index is None or not 0 <= index < 1024 or flag_value is None:
                state.skipped_flags += 1
            elif flag_value != 0:
                state.game_flags[index] = flag_value
            continue

        if key in {"visual_cg", "h_cg", "replay"}:
            unlock = parse_int(value)
            if unlock is None:
                state.skipped_unlocks += 1
            else:
                kind = "visual_cg" if key == "visual_cg" else key
                state.unlocks.add((kind, unlock))
            continue

        state.cleaned_lines.append(raw_line)
    return state


def initialize_database(connection: sqlite3.Connection) -> None:
    connection.executescript(
        """
        PRAGMA foreign_keys = ON;
        CREATE TABLE IF NOT EXISTS read_lines (
            script TEXT NOT NULL,
            position INTEGER NOT NULL,
            revealed_count INTEGER NOT NULL,
            PRIMARY KEY (script, position, revealed_count)
        );
        CREATE TABLE IF NOT EXISTS game_flags (
            idx INTEGER PRIMARY KEY CHECK (idx >= 0 AND idx < 1024),
            value INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS unlocks (
            kind TEXT NOT NULL,
            id INTEGER NOT NULL,
            PRIMARY KEY (kind, id)
        );
        PRAGMA user_version = 1;
        """
    )


def write_database(path: Path, state: MigratedState, replace: bool) -> None:
    if replace and path.exists():
        backup = path.with_suffix(path.suffix + ".bak")
        shutil.copy2(path, backup)
    path.parent.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(path) as connection:
        initialize_database(connection)
        with connection:
            connection.executemany(
                """
                INSERT OR IGNORE INTO read_lines
                    (script, position, revealed_count)
                VALUES (?, ?, ?)
                """,
                sorted(state.read_lines),
            )
            connection.executemany(
                """
                INSERT INTO game_flags (idx, value)
                VALUES (?, ?)
                ON CONFLICT(idx) DO UPDATE SET value = excluded.value
                """,
                sorted(state.game_flags.items()),
            )
            connection.executemany(
                """
                INSERT OR IGNORE INTO unlocks (kind, id)
                VALUES (?, ?)
                """,
                sorted(state.unlocks),
            )


def write_clean_ini(path: Path, lines: list[str]) -> None:
    if path.exists():
        backup = path.with_suffix(path.suffix + ".bak")
        shutil.copy2(path, backup)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Migrate legacy read/game-flag/unlock keys from a ToHeart2 "
            ".ini file into the SQLite state database."
        )
    )
    parser.add_argument(
        "ini",
        nargs="?",
        type=Path,
        help=(
            "legacy config .ini to read "
            "(default: toheart2-config.ini if present, otherwise "
            "profile/toheart2-config.ini)"
        ),
    )
    parser.add_argument(
        "--state",
        type=Path,
        default=Path("profile/toheart2-state.sqlite3"),
        help="SQLite state DB to write (default: profile/toheart2-state.sqlite3)",
    )
    parser.add_argument(
        "--replace-db",
        action="store_true",
        help="allow updating an existing DB, backing it up as *.bak first",
    )
    parser.add_argument(
        "--clean-ini",
        type=Path,
        default=Path("profile/toheart2-config.ini"),
        help=(
            "write an .ini without migrated state keys to this path; existing "
            "files are backed up as *.bak "
            "(default: profile/toheart2-config.ini)"
        ),
    )
    arguments = parser.parse_args()
    if arguments.ini is None:
        arguments.ini = (
            Path("toheart2-config.ini")
            if Path("toheart2-config.ini").exists()
            else Path("profile/toheart2-config.ini")
        )

    if not arguments.ini.is_file():
        print(f"{arguments.ini} does not exist", file=sys.stderr)
        return 1
    if arguments.state.exists() and not arguments.replace_db:
        print(
            f"{arguments.state} already exists; pass --replace-db to update it",
            file=sys.stderr,
        )
        return 1

    state = parse_ini(arguments.ini)
    write_database(arguments.state, state, arguments.replace_db)
    if arguments.clean_ini:
        write_clean_ini(arguments.clean_ini, state.cleaned_lines)

    print(f"Read lines migrated: {len(state.read_lines)}")
    print(f"Game flags migrated: {len(state.game_flags)}")
    print(f"Unlocks migrated:    {len(state.unlocks)}")
    if state.skipped_reads or state.skipped_flags or state.skipped_unlocks:
        print(
            "Skipped invalid rows: "
            f"read={state.skipped_reads}, "
            f"flags={state.skipped_flags}, "
            f"unlocks={state.skipped_unlocks}",
            file=sys.stderr,
        )
    if arguments.clean_ini:
        print(f"Cleaned ini written: {arguments.clean_ini}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
