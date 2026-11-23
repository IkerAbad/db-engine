# db-engine

[![ci](https://github.com/IkerAbad/db-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/IkerAbad/db-engine/actions/workflows/ci.yml)
[![license](https://img.shields.io/badge/license-MIT-333?style=flat-square)](LICENSE)
![c++23](https://img.shields.io/badge/C%2B%2B-23-333?style=flat-square)
![version](https://img.shields.io/badge/version-0.2.0-333?style=flat-square)

A relational storage engine and SQL executor in C++23. It uses 4 KB pages, an
LRU buffer pool, a B+tree, a write-ahead log and crash recovery. There are no
external dependencies: the pager talks to the operating system, everything else
is in this repository.

The engine is single-threaded by design. It is meant to show the mechanisms,
not to compete with SQLite.

## What is implemented

- 4 KB slotted pages with CRC32 checksums and a per-page LSN.
- LRU buffer pool with pinning, dirty write-back, hit and miss counters.
- B+tree with insert, delete, split, merge, redistribution and ordered forward
  and reverse scans. Keys are signed 64-bit integers.
- Fixed-schema rows serialized to little-endian binary: `INT`, `DOUBLE`,
  `BOOL`, `TEXT(n)`.
- Write-ahead log with `BEGIN`, `INSERT`, `DELETE`, `COMMIT`, `ABORT` and
  `CHECKPOINT` records. Full page images are appended when a dirty page is
  written, so a torn page can be repaired.
- Checkpoints and automatic recovery at startup.
- SQL parser and a volcano-model executor: table and index scans, filter,
  projection, sort (spills to disk) and limit iterators.
- Inner hash join of two tables on one equality, with qualified column
  resolution and a bounded build side.
- Interactive shell and script mode.
- 32 unit tests, 44 crash-recovery scenarios and a 100k insert benchmark.

## Quick start

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/db_engine --db example.db
```

```
db> CREATE TABLE users (id INT PRIMARY KEY, name TEXT(16), score DOUBLE);
table users created
db> INSERT INTO users VALUES (1, 'alice', 9.5), (2, 'bob', 7.25);
2 rows inserted
db> SELECT * FROM users ORDER BY score DESC LIMIT 1;
id | name  | score
---+-------+------
1  | alice | 9.5
(1 row)
db> .exit
```

Run a script with `--script file.sql`, run one query with `--query "..."`, or
check the files with `--check`.

## SQL subset

```sql
CREATE TABLE name (
    id INT PRIMARY KEY,
    title TEXT(40),
    amount DOUBLE,
    active BOOL
);
DROP TABLE [IF EXISTS] name;

INSERT INTO name VALUES (1, 'one', 1.5, true), (2, 'two', 2.5, false);

SELECT * FROM name
WHERE id >= 10 AND id < 20
ORDER BY id DESC
LIMIT 5;

SELECT COUNT(*) FROM name WHERE active = true;

SELECT users.id, orders.total
FROM users
INNER JOIN orders ON users.id = orders.user_id
WHERE orders.total > 10
ORDER BY users.id
LIMIT 5;

DELETE FROM name WHERE id = 3;

BEGIN;
INSERT INTO name VALUES (3, 'three', 3.5, true);
ROLLBACK;
```

Supported predicates are comparisons on a single column joined by `AND`:
`=`, `!=`, `<`, `<=`, `>`, `>=`. `ORDER BY` takes one column and an optional
`ASC` or `DESC`. `LIMIT` takes a non-negative integer. `COUNT(*)` is the only
aggregate. The primary key must be `INT`, every `INSERT` must provide every
column, and `NULL` is not supported.

The join form is an inner equijoin of two tables. Qualified names are
`table.column`. An unqualified name is accepted only when it exists in exactly
one of the two tables; otherwise the engine reports an ambiguous column.
`SELECT *` returns the left table columns followed by the right table columns,
labelled `table.column`. The right table is the build side and the engine does
not reorder the join. Table aliases are not supported, so a table cannot be
joined to itself.

`WHERE` predicates on the primary key become B+tree range bounds. Other
predicates are evaluated by a filter iterator. `ORDER BY` on the primary key
uses the index order and needs no sort. Sorting any other column uses an
external merge sort with a fixed buffer, so memory stays bounded even when the
result is large. A join uses full table scans, not B+tree range bounds.

## Building and testing

Requirements: CMake 3.25 or newer, a C++23 compiler, and Python 3 for the
recovery tests. Ninja is optional.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

`ctest` runs the unit tests, the crash-recovery suite and, in a Release build,
the 100k row benchmark. The recovery suite starts real child processes, kills
them at deterministic points and reopens the database. It also kills a process
at a random moment five times.

The same build is exercised on Linux with GCC and AddressSanitizer/UBSan, and
on Windows with MSVC. A separate recovery job runs only the crash suite, so the
process-kill tests cannot silently disappear from the main test run.

## Architecture

### Files

A database is a directory with three files:

| File | Contents |
|---|---|
| `data.db` | 4 KB pages: page 0 is the allocator header, one meta page per table, then B+tree pages |
| `wal.log` | append-only write-ahead log with checksummed records |
| `catalog.db` | atomic snapshot of table names, schemas, table meta pages and the checkpoint offset |

Page 0 stores the next unused page id. Each table has a fixed meta page that
stores the current B+tree root and the first leaf. Because the meta page never
moves, a root split only changes one 4 KB page. A table's leaf pages are linked
through `next` and `prev` pointers, which is what ordered scans follow.

### Pages and buffer pool

Pages have a 64-byte header, a sorted slot directory and cells that grow from
the end of the page. `TEXT` values are fixed width, so every row has the same
size. Insertions compact the page when the free space is fragmented.

The buffer pool preallocates a fixed number of frames (4096 by default, 16 MiB)
and evicts the least recently used unpinned page. Before a dirty page is
written, its full 4 KB image is appended to the WAL and the WAL is flushed. The
data file is written afterwards. That ordering is the write-ahead rule, and it
also gives recovery a complete copy of every page that was ever written.

### B+tree

Leaves store `(int64 key, row bytes)` cells. Internal pages store a leftmost
child plus `(separator key, right child)` cells, where the separator is the
minimum key of the right subtree. Insert splits a full page and flushes the new
sibling before linking it. Delete rebalances underfull pages with a sibling:
it redistributes cells when the sibling has room, otherwise it merges the two
pages and removes the parent separator. The root collapses when it has no
separators left.

The meta page stores the first leaf as well as the root. The leaf chain is
therefore reachable even if the internal levels were written in a different
order. Recovery uses the chain as the source of truth, as described below.

### Hash join

`SELECT ... FROM left [INNER] JOIN right ON left.col = right.col` runs as a
single-threaded in-memory hash join:

1. Scan the right table, compute the join key from the `ON` column and append the
   encoded right row to a hash bucket.
2. Scan the left table and probe the bucket for every left row.
3. Emit the left row followed by each matching right row. The result is passed
   to the normal filter, sort, limit and projection iterators.
4. If the build side exceeds `join_buffer_bytes` (64 MiB by default,
   `--join-buffer` on the command line), the query fails with an explicit
   error. There is no hash join spilling.

The right table is always the build side and the plan is not reordered. Both
tables are read with full scans, because the engine has no secondary indexes.
The single-table path still uses B+tree range bounds. `WHERE` runs after the
join and `ORDER BY` uses the external merge sort described above.

### Transactions and WAL

Each statement outside an explicit transaction is its own transaction. Inside
`BEGIN ... COMMIT`, all changes share one transaction id. The log records the
old row for a delete and the new row for an insert, so rollback and recovery
can apply the inverse operation.

Commit appends a `COMMIT` record and calls `fsync` on the WAL. Data pages are
not forced at commit: the row records are enough to replay the transaction if
the process dies before the pages reach disk. A checkpoint flushes all dirty
pages, fsyncs the data file, appends a checkpoint record and writes a new
catalog snapshot.

Rollback walks the WAL backwards, applies the inverse row operation for every
record of the transaction, flushes the pages, then appends `ABORT`. The reverse
walk is possible because every record stores the end offset of the previous
record.

### Recovery

At startup the engine compares the WAL size with the checkpoint offset stored in
the catalog. If there are records after the checkpoint it runs these steps:

1. Replay page images whose LSN is newer than the page on disk. A page whose
   checksum fails is treated as empty and is restored from its image.
2. Replay committed `CREATE TABLE` and `DROP TABLE` records.
3. Rebuild each table's internal B+tree levels from its leaf chain. Page writes
   can interleave with splits and merges, so the chain, not the internal
   pointers, defines the set of leaves.
4. Replay committed inserts and deletes in log order. Both operations are
   idempotent, so applying one twice is harmless.
5. Undo every transaction that has a `BEGIN` but no `COMMIT` or `ABORT`, walking
   the log backwards and applying inverse row operations.
6. Flush the data file, append `ABORT` records for the losers, append a
   checkpoint and save the catalog.

This is not a full ARIES implementation. It uses physical redo for page images
and logical redo and undo for rows, then repairs the index structure from the
leaf chain. That is enough for the crash points below, including a write that
is interrupted halfway through a page.

### Crash tests

`DBENGINE_CRASH_AT` selects a fault point and the engine calls `_Exit(86)` when
it is reached. `DBENGINE_CRASH_COUNT` selects the nth occurrence. The recovery
suite covers:

- before and after a WAL append, before and after a WAL fsync;
- before the commit record, after it is appended and after it is synced;
- before a page write, after its WAL image, before the data write, and a
  deliberate half-page write;
- every phase of a checkpoint and of a rollback;
- every phase of recovery itself, including a crash during a multi-leaf rebuild;
- a WAL tail truncated in the middle of a record;
- five real `SIGKILL`/`TerminateProcess` kills at random times.

Every case reopens the database, runs `--check`, and verifies that the row count
is one of the two valid transaction boundaries and that no partial transaction
is visible.

## Benchmark

Measured on 2026-11-23 with the Release build produced by GCC 14.2.0 (MSYS2
UCRT64) on Windows 11, an AMD Ryzen 9 5900X, 32 GB of DDR4-3600 memory and an
NTFS volume:

| Metric | Result |
|---|---|
| Inserts per second (100,000 rows, one transaction) | 14,455 |
| Range queries per second (1,000 queries, 100 rows each) | 30,566 |
| Point lookups per second | 368,718 |
| Peak resident memory | 25.20 MiB |

```bash
./build/db_engine --bench --rows 100000 --queries 1000 --buffer-pages 4096
```

```
operation                      ops      p50 ms      p99 ms     mean ms         ops/s
insert (one txn)            100000      0.0071      2.5367      0.0685         14455
insert (autocommit)            200      2.3781      7.1904      2.5960           385
range scan (100 rows)         1000      0.0285      0.0551      0.0327         30566
point lookup                  1000      0.0025      0.0049      0.0027        368718
count(*)                         1     25.5899     25.5899     25.5899            39

commit (batch)          2.30 ms
peak RSS                25.20 MB
data file               8.77 MB (2246 pages)
wal file                19.63 MB
buffer hits/misses      1613868 / 2246 (99.9%)
integrity               ok
```

The benchmark inserts 100,000 keys in random order inside one transaction. The
latency is measured around each SQL execution call, including parsing. The p99
of the insert path is a leaf split, which flushes the new sibling before it is
linked. The autocommit row measures one `fsync` per insert, which is the honest
cost of an individual committed statement. The range scan measures a plan that
seeks into the B+tree and returns 100 rows.

Memory is bounded by construction: a 4096-frame buffer pool (16 MiB), a 4 MiB
sort buffer and one row per iterator. A join query can additionally hold up to
`join_buffer_bytes` (64 MiB) for its build side, and that is where it fails if
the right table does not fit. The 25 MiB peak is the single-table benchmark
above. With `--buffer-pages 256` the 10,000-row run peaks at 7.73 MiB, at the
cost of more page writes and a lower insert throughput.

## Why not SQLite

This project demonstrates the machinery that a relational database needs and
that a high-level API hides: a 4 KB slotted page format, an LRU buffer pool, a
B+tree that splits and merges, a write-ahead log that orders page images and row
records, checkpoints, and a recovery path tested by killing the process at
every step of the commit and page-write sequence. The benchmark and the crash
suite are part of the deliverable because they turn those mechanisms into
something measurable and falsifiable rather than a diagram in a README.

It does not compete with SQLite, and it is not a replacement for any production
database. SQLite has decades of hardening, a query planner, secondary indexes,
joins, concurrent readers, a huge compatibility and test surface, and an
ecosystem around it. This engine is single-threaded, has one active transaction,
only two-table inner joins, no secondary indexes, never reuses pages, and
rebuilds its B+tree during recovery. Its purpose is to show that the mechanisms
can be built and verified from scratch, not to store anyone's real data.

## What is deliberately missing

- One thread, one active transaction and one process at a time. No MVCC,
  locking or isolation levels.
- Only inner equijoins of two tables. No outer joins, multi-table joins, join
  reordering, hash join spilling or table aliases. The right table is always the
  build side.
- No secondary indexes, `UPDATE`, `GROUP BY`, `OR`, subqueries or `ALTER TABLE`.
- No `NULL`, no defaults. Every `INSERT` must supply every column.
- DDL is autocommit and cannot run inside an explicit transaction.
- Page ids are never reused. Merged or dropped pages stay allocated until the
  database is copied or rebuilt.
- The WAL is never truncated. Checkpoints reduce the amount of redo work, not
  the file size.
- `ORDER BY` handles one column.
- `DELETE` collects the matching rows before deleting them, so its memory is
  proportional to the number of deleted rows.
- Recovery rebuilds each table's B+tree from the leaf chain. It is O(n log n)
  and is not a full ARIES redo/undo implementation.
- A corrupt record in the middle of the WAL is treated like a torn tail: the
  log is truncated at the last valid record.

## Layout

```
include/db/     public headers for the storage engine and SQL layer
src/            pager, WAL, B+tree, catalog, parser, executor and CLI
tests/          unit tests and the Python crash-recovery suite
.github/        CI workflow
```

## License

MIT. Copyright (c) 2026 Iker Abad.
