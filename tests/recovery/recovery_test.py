#!/usr/bin/env python3
"""Crash-recovery tests for db-engine.

The engine injects deterministic faults through DBENGINE_CRASH_AT. Every case
runs a child process against a copy of a known database, kills it at a chosen
point, reopens the database and verifies:

  * the database passes the page and B+tree integrity check;
  * the row count is one of the valid transaction boundaries (never partial);
  * every row that is present has the expected value.

A random-kill case uses TerminateProcess/SIGKILL to catch races that a fixed
fault point cannot express.
"""

from __future__ import annotations

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BASE_ROWS = 20
EXTRA_ROWS = 10
CRASH_EXIT = 86


class Failure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def write_script(path: Path, start: int, stop: int, ending: str) -> None:
    lines = ["BEGIN;"]
    for key in range(start, stop):
        lines.append(f"INSERT INTO items VALUES ({key}, {key * 2});")
    lines.append(ending)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run(
    exe: Path,
    db: Path,
    *,
    script: Path | None = None,
    query: str | None = None,
    check: bool = False,
    crash: str | None = None,
    crash_count: int = 1,
    buffer_pages: int = 4096,
    timeout: int = 120,
) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env.pop("DBENGINE_CRASH_AT", None)
    env.pop("DBENGINE_CRASH_COUNT", None)
    if crash is not None:
        env["DBENGINE_CRASH_AT"] = crash
        env["DBENGINE_CRASH_COUNT"] = str(crash_count)
    args = [str(exe), "--db", str(db), "--buffer-pages", str(buffer_pages)]
    if check:
        args.append("--check")
    if script is not None:
        args += ["--script", str(script)]
    if query is not None:
        args += ["--query", query, "--values"]
    return subprocess.run(
        args,
        cwd=str(db.parent),
        env=env,
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def query_rows(exe: Path, db: Path, query: str) -> list[list[str]]:
    result = run(exe, db, query=query)
    if result.returncode != 0:
        raise Failure(
            f"query failed ({result.returncode}): {query}\n{result.stderr}"
        )
    rows = []
    for line in result.stdout.splitlines():
        line = line.rstrip("\r")
        if not line:
            continue
        rows.append(line.split("\t"))
    return rows


def verify(exe: Path, db: Path, allowed_counts: set[int]) -> int:
    check = run(exe, db, check=True)
    if check.returncode != 0:
        raise Failure(f"integrity check failed: {check.stderr}{check.stdout}")
    rows = query_rows(exe, db, "SELECT id, v FROM items ORDER BY id")
    count = len(rows)
    require(count in allowed_counts, f"invalid row count {count}, expected {sorted(allowed_counts)}")
    for index, row in enumerate(rows):
        require(len(row) == 2, f"bad row shape: {row}")
        key = int(row[0])
        value = int(row[1])
        require(key == index, f"row {index} has id {key}: partial transaction")
        require(value == key * 2, f"row {key} has value {value}, expected {key * 2}")
    return count


def make_base(exe: Path, work: Path) -> Path:
    db = work / "base"
    if db.exists():
        shutil.rmtree(db)
    script = work / "base.sql"
    script.write_text(
        "CREATE TABLE items (id INT PRIMARY KEY, v INT);\n"
        + "\n".join(f"INSERT INTO items VALUES ({key}, {key * 2});" for key in range(BASE_ROWS))
        + "\n",
        encoding="utf-8",
    )
    result = run(exe, db, script=script)
    require(result.returncode == 0, f"cannot create the base database: {result.stderr}")
    verify(exe, db, {BASE_ROWS})
    return db


def fresh_case(base: Path, work: Path, name: str) -> Path:
    case = work / name
    if case.exists():
        shutil.rmtree(case)
    shutil.copytree(base, case)
    return case


def make_large_base(exe: Path, work: Path) -> Path:
    db = work / "large"
    if db.exists():
        shutil.rmtree(db)
    script = work / "large.sql"
    lines = ["CREATE TABLE items (id INT PRIMARY KEY, v INT);", "BEGIN;"]
    for key in range(3000):
        lines.append(f"INSERT INTO items VALUES ({key}, {key * 2});")
    lines.append("COMMIT;")
    script.write_text("\n".join(lines) + "\n", encoding="utf-8")
    result = run(exe, db, script=script)
    require(result.returncode == 0, f"cannot create the large base database: {result.stderr}")
    verify(exe, db, {3000})
    return db


def crash_case(
    exe: Path,
    base: Path,
    work: Path,
    name: str,
    crash: str,
    *,
    ending: str = "COMMIT;",
    start: int = BASE_ROWS,
    stop: int = BASE_ROWS + EXTRA_ROWS,
    buffer_pages: int = 4096,
    crash_count: int = 1,
    allowed_counts: set[int] | None = None,
) -> int:
    db = fresh_case(base, work, name)
    script = work / f"{name}.sql"
    write_script(script, start, stop, ending)
    result = run(
        exe,
        db,
        script=script,
        crash=crash,
        crash_count=crash_count,
        buffer_pages=buffer_pages,
    )
    require(
        result.returncode == CRASH_EXIT,
        f"{name}: expected crash exit {CRASH_EXIT}, got {result.returncode}\n"
        f"stdout={result.stdout[-2000:]}\nstderr={result.stderr[-2000:]}",
    )
    if allowed_counts is None:
        allowed_counts = {BASE_ROWS, BASE_ROWS + EXTRA_ROWS}
    return verify(exe, db, allowed_counts)


def recovery_crash_case(
    exe: Path,
    base: Path,
    work: Path,
    name: str,
    setup_crash: str,
    recovery_crash: str,
    *,
    start: int = BASE_ROWS,
    stop: int = BASE_ROWS + EXTRA_ROWS,
    buffer_pages: int = 4096,
    allowed_counts: set[int] | None = None,
) -> int:
    db = fresh_case(base, work, name)
    script = work / f"{name}.sql"
    write_script(script, start, stop, "COMMIT;")
    setup = run(
        exe,
        db,
        script=script,
        crash=setup_crash,
        buffer_pages=buffer_pages,
    )
    require(
        setup.returncode == CRASH_EXIT,
        f"{name}: setup did not crash (exit {setup.returncode})",
    )
    middle = run(exe, db, crash=recovery_crash)
    require(
        middle.returncode == CRASH_EXIT,
        f"{name}: recovery did not crash at {recovery_crash} "
        f"(exit {middle.returncode})\n{middle.stderr}",
    )
    if allowed_counts is None:
        allowed_counts = {BASE_ROWS, BASE_ROWS + EXTRA_ROWS}
    return verify(exe, db, allowed_counts)


def torn_tail_case(
    exe: Path,
    base: Path,
    work: Path,
    name: str,
    cut: int,
) -> int:
    db = fresh_case(base, work, name)
    script = work / f"{name}.sql"
    write_script(script, BASE_ROWS, BASE_ROWS + EXTRA_ROWS, "COMMIT;")
    setup = run(exe, db, script=script, crash="txn.commit.after_sync")
    require(setup.returncode == CRASH_EXIT, f"{name}: setup did not crash")
    wal = db / "wal.log"
    size = wal.stat().st_size
    require(size > cut, f"{name}: wal is too short to cut {cut} bytes")
    with wal.open("r+b") as handle:
        handle.truncate(size - cut)
    return verify(exe, db, {BASE_ROWS, BASE_ROWS + EXTRA_ROWS})


def random_kill_case(
    exe: Path,
    base: Path,
    work: Path,
    name: str,
    delay: float,
) -> int:
    db = fresh_case(base, work, name)
    script = work / f"{name}.sql"
    write_script(script, BASE_ROWS, BASE_ROWS + 2000, "COMMIT;")
    args = [str(exe), "--db", str(db), "--buffer-pages", "16", "--script", str(script)]
    process = subprocess.Popen(
        args,
        cwd=str(db.parent),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(delay)
    try:
        process.kill()
    except OSError:
        pass
    process.wait(timeout=30)
    return verify(exe, db, {BASE_ROWS, BASE_ROWS + 2000})


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--workdir", required=True, type=Path)
    args = parser.parse_args()

    exe = args.exe.resolve()
    work = args.workdir.resolve()
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True, exist_ok=True)

    failures: list[str] = []
    passes = 0

    def case(name: str, function) -> None:
        nonlocal passes
        try:
            count = function()
            print(f"ok   {name:<38} rows={count}")
            passes += 1
        except Exception as error:  # noqa: BLE001 - the harness reports everything
            failures.append(f"{name}: {error}")
            print(f"FAIL {name:<38} {error}")

    try:
        base = make_base(exe, work)

        # A crash before or while the WAL record is written leaves a loser.
        case("wal.append.before", lambda: crash_case(
            exe, base, work, "wal-append-before", "wal.append.before",
            allowed_counts={BASE_ROWS}))
        case("wal.append.after (BEGIN)", lambda: crash_case(
            exe, base, work, "wal-append-after", "wal.append.after",
            allowed_counts={BASE_ROWS}))
        # The COMMIT record was appended but not necessarily synced; a process
        # crash keeps it in the page cache, so either outcome is valid.
        case("wal.sync.before", lambda: crash_case(
            exe, base, work, "wal-sync-before", "wal.sync.before",
            allowed_counts={BASE_ROWS, BASE_ROWS + EXTRA_ROWS}))
        case("wal.sync.after", lambda: crash_case(
            exe, base, work, "wal-sync-after", "wal.sync.after",
            allowed_counts={BASE_ROWS + EXTRA_ROWS}))
        case("txn.commit.before", lambda: crash_case(
            exe, base, work, "commit-before", "txn.commit.before",
            allowed_counts={BASE_ROWS}))
        case("txn.commit.after_log", lambda: crash_case(
            exe, base, work, "commit-after-log", "txn.commit.after_log",
            allowed_counts={BASE_ROWS, BASE_ROWS + EXTRA_ROWS}))
        case("txn.commit.after_sync", lambda: crash_case(
            exe, base, work, "commit-after-sync", "txn.commit.after_sync",
            allowed_counts={BASE_ROWS + EXTRA_ROWS}))

        # A crash while a dirty page is written must be repaired by the page
        # image that the WAL wrote before the data file.
        for point in [
            "page.write.half",
            "page.write.before_wal",
            "page.write.after_wal",
            "page.write.before_data",
            "page.write.after",
        ]:
            case(point, lambda point=point: crash_case(
                exe, base, work, point.replace(".", "-"), point,
                start=BASE_ROWS,
                stop=BASE_ROWS + 2000,
                buffer_pages=8,
                allowed_counts={BASE_ROWS, BASE_ROWS + 2000}))

        # Crash during the checkpoint that runs on close.
        for point in [
            "flush.all.before",
            "flush.all.after_wal",
            "flush.half_write",
            "flush.all.after",
            "checkpoint.before",
            "checkpoint.after_flush",
            "checkpoint.after_log",
            "checkpoint.after_catalog",
        ]:
            case(point, lambda point=point: crash_case(
                exe, base, work, point.replace(".", "-"), point,
                allowed_counts={BASE_ROWS + EXTRA_ROWS}))

        # Crash during an explicit rollback.
        for point in [
            "txn.rollback.before",
            "undo.before",
            "undo.during",
            "undo.after",
            "txn.rollback.after_undo",
            "txn.rollback.after_abort",
        ]:
            case(point, lambda point=point: crash_case(
                exe, base, work, point.replace(".", "-"), point,
                ending="ROLLBACK;", allowed_counts={BASE_ROWS}))

        # Crash while recovering a committed transaction.
        for point in [
            "recovery.before",
            "recovery.after_redo",
            "recovery.after_rebuild",
            "recovery.after_row_redo",
            "recovery.after_undo",
            "recovery.after_checkpoint",
        ]:
            case(f"recover {point}", lambda point=point: recovery_crash_case(
                exe, base, work, "recover-" + point.replace(".", "-"),
                "txn.commit.after_sync", point,
                allowed_counts={BASE_ROWS + EXTRA_ROWS}))

        # Crash while recovering a loser (uncommitted transaction).
        case("recover loser after undo", lambda: recovery_crash_case(
            exe, base, work, "recover-loser",
            "page.write.half", "recovery.after_undo",
            start=BASE_ROWS, stop=BASE_ROWS + 2000, buffer_pages=8,
            allowed_counts={BASE_ROWS}))

        # Rebuilding a multi-leaf tree during recovery must be crash-safe.
        large = make_large_base(exe, work)
        case("recover large tree", lambda: recovery_crash_case(
            exe, large, work, "recover-large",
            "txn.commit.after_sync", "recovery.after_rebuild",
            start=3000, stop=3010, allowed_counts={3010}))

        # A torn WAL tail must be discarded without losing committed rows.
        for cut in [1, 4, 16, 200, 1000]:
            case(f"torn wal tail -{cut}", lambda cut=cut: torn_tail_case(
                exe, base, work, f"torn-{cut}", cut))

        # Real kill at a random point, five times.
        rng = random.Random(1234)
        for index in range(5):
            delay = rng.uniform(0.02, 0.20)
            case(f"random kill #{index + 1} ({delay:.3f}s)", lambda delay=delay, index=index: random_kill_case(
                exe, base, work, f"random-kill-{index}", delay))

    except Exception as error:  # noqa: BLE001
        failures.append(f"setup: {error}")
        print(f"FAIL setup: {error}")

    print(f"\n{passes} passed, {len(failures)} failed")
    if failures:
        for failure in failures:
            print(f"  {failure}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
