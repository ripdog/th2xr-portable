#!/usr/bin/env python3
"""Run independent ToHeart2 soak routes concurrently and merge the results."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

from soak_state import SoakState, merge_state, parse_state, route_key, write_state


class CoordinatorLock:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.fd: int | None = None

    def __enter__(self) -> "CoordinatorLock":
        try:
            self.fd = os.open(
                self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644
            )
        except FileExistsError as error:
            raise RuntimeError(
                f"parallel soak coordinator already active: {self.path}"
            ) from error
        os.write(self.fd, f"{os.getpid()}\n".encode())
        return self

    def __exit__(self, *_: object) -> None:
        if self.fd is not None:
            os.close(self.fd)
        self.path.unlink(missing_ok=True)


def worker_state(global_state: SoakState, route: str) -> SoakState:
    return SoakState(
        pending={route},
        completed=set(global_state.completed),
        nodes=dict(global_state.nodes),
        pending_records=1,
    )


def merge_worker(
    global_state: SoakState, worker_directory: Path, leased_route: str
) -> None:
    state_path = worker_directory / "state.txt"
    if not state_path.exists():
        global_state.pending.add(leased_route)
        return
    merge_state(global_state, parse_state(state_path))


def append_worker_log(global_log: Path, worker_directory: Path) -> None:
    worker_log = worker_directory / "runs.log"
    if not worker_log.exists():
        return
    global_log.parent.mkdir(parents=True, exist_ok=True)
    with global_log.open("a", encoding="utf-8") as output:
        output.write(worker_log.read_text(encoding="utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Explore independent soak routes with multiple processes."
    )
    parser.add_argument(
        "game_data",
        nargs="?",
        type=Path,
        default=Path("game-data"),
    )
    parser.add_argument(
        "--engine",
        type=Path,
        default=Path("build/toheart2"),
        help="engine executable (default: build/toheart2)",
    )
    parser.add_argument(
        "--state",
        type=Path,
        default=Path("logs/soak"),
        help="shared campaign directory (default: logs/soak)",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=min(4, os.cpu_count() or 1),
        help="concurrent engine processes (default: min(4, CPU count))",
    )
    parser.add_argument(
        "--runs",
        type=int,
        default=20,
        help="maximum total routes for this invocation (default: 20)",
    )
    arguments = parser.parse_args()

    if arguments.workers <= 0:
        parser.error("--workers must be positive")
    if arguments.runs <= 0:
        parser.error("--runs must be positive")
    if not arguments.engine.is_file():
        parser.error(f"engine not found: {arguments.engine}")

    arguments.state.mkdir(parents=True, exist_ok=True)
    state_path = arguments.state / "state.txt"
    if not state_path.exists():
        write_state(state_path, SoakState(pending={""}, pending_records=1))

    lock_path = arguments.state / "parallel.lock"
    workers_root = arguments.state / "workers"
    completed = 0

    try:
        with CoordinatorLock(lock_path):
            while completed < arguments.runs:
                state = parse_state(state_path)
                state.pending.update(state.active)
                state.active.clear()
                state.pending.difference_update(state.completed)
                available = sorted(state.pending, key=route_key)
                batch_size = min(
                    arguments.workers,
                    arguments.runs - completed,
                    len(available),
                )
                if batch_size == 0:
                    break

                leased = available[:batch_size]
                for route in leased:
                    state.pending.remove(route)
                    state.active.add(route)
                write_state(state_path, state)

                processes: list[
                    tuple[int, str, Path, subprocess.Popen[bytes]]
                ] = []
                for index, route in enumerate(leased):
                    worker_directory = workers_root / f"worker-{index}"
                    shutil.rmtree(worker_directory, ignore_errors=True)
                    worker_directory.mkdir(parents=True)
                    write_state(
                        worker_directory / "state.txt",
                        worker_state(state, route),
                    )
                    print(
                        f"[worker {index}] starting route "
                        f"{route or '<root>'}",
                        flush=True,
                    )
                    command = [
                        str(arguments.engine.resolve()),
                        str(arguments.game_data.resolve()),
                        "--soak-state",
                        str(worker_directory.resolve()),
                        "--soak-runs",
                        "1",
                    ]
                    processes.append(
                        (
                            index,
                            route,
                            worker_directory,
                            subprocess.Popen(command),
                        )
                    )

                try:
                    for index, route, worker_directory, process in processes:
                        return_code = process.wait()
                        print(
                            f"[worker {index}] exited {return_code}: "
                            f"{route or '<root>'}",
                            flush=True,
                        )
                except KeyboardInterrupt:
                    for _, _, _, process in processes:
                        if process.poll() is None:
                            process.terminate()
                    for _, _, _, process in processes:
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
                    raise

                state = parse_state(state_path)
                for index, route, worker_directory, process in processes:
                    state.active.discard(route)
                    merge_worker(state, worker_directory, route)
                    append_worker_log(
                        arguments.state / "runs.log", worker_directory
                    )
                    completed += 1
                state.pending.difference_update(state.completed)
                state.pending_records = len(state.pending)
                write_state(state_path, state)
                print(
                    f"parallel soak: {completed}/{arguments.runs} routes "
                    f"finished, {len(state.remaining)} known remaining",
                    flush=True,
                )
    except KeyboardInterrupt:
        print("parallel soak interrupted", file=sys.stderr)
        return 130
    except (OSError, RuntimeError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
