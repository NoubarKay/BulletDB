<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/bulletdb-lockup-white-red.png">
    <img src="docs/bulletdb-lockup.png" alt="BulletDB" width="420">
  </picture>
</p>

<p align="center">
  <em>A personal learning project: a columnar database engine built from scratch, one step at a time.<br>
  Issues and discussion are welcome; pull requests may not be accepted.</em>
</p>

<p align="center">
  <a href="https://github.com/NoubarKay/BulletDB/actions/workflows/ci.yml"><img src="https://github.com/NoubarKay/BulletDB/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI"></a>
  <a href="https://github.com/NoubarKay/BulletDB/actions/workflows/codeql.yml"><img src="https://github.com/NoubarKay/BulletDB/actions/workflows/codeql.yml/badge.svg?branch=main" alt="CodeQL"></a>
  <a href="https://github.com/NoubarKay/BulletDB/tags"><img src="https://img.shields.io/github/v/tag/NoubarKay/BulletDB?label=version&sort=semver" alt="Latest version"></a>
  <img src="https://img.shields.io/badge/C-23-00599C?logo=c&logoColor=white" alt="C23">
  <img src="https://img.shields.io/badge/platforms-Linux%20%7C%20Windows-lightgrey" alt="Platforms: Linux, Windows">
  <img src="https://img.shields.io/badge/status-experimental-orange" alt="Status: experimental">
  <a href="https://github.com/NoubarKay/BulletDB/commits/main"><img src="https://img.shields.io/github/last-commit/NoubarKay/BulletDB" alt="Last commit"></a>
</p>

> [!WARNING]
> **Not production ready.** BulletDB is an early-stage research project. The
> file format, APIs and on-disk layout will change without notice. Don't use
> it to store data you care about.

**BulletDB** is a columnar OLAP (analytical) database engine written in C.

It's built for analytical queries: scanning, filtering and aggregating large
tables. Data is stored **by column**, so a query on `price` never reads any
other column, and each column's values sit next to each other in memory, which
keeps scan loops tight and cache-friendly. The design is modeled loosely on
DuckDB and Parquet.

## What works today

- **CSV import** that streams a file in chunks of up to 2048 typed rows
  (`INT`, `DOUBLE`, `BOOL`), with NULLs
- **A columnar file format (`.bdb`)** with row groups, NULL bitmaps, and a
  footer that indexes every column block
- **A streaming reader** that holds only the footer and one chunk in memory,
  whatever the file size
- **A vectorized query executor** (Volcano model, one chunk per call):
  `SCAN` → `FILTER` → `AGGREGATE`, with `WHERE column op value` on `INT` and
  `DOUBLE` columns (selection vectors, no copying), and `SUM`, `COUNT`,
  `MIN`, `MAX`, `AVG`
- **`EXPLAIN`-style plans** with per-operator statistics (calls, chunks, rows,
  time)
- **Tests and CI** on Linux, Linux with sanitizers, and Windows

Next up: per-group statistics (format v2) and zone-map pruning, so a filter
can skip whole row groups. See the [roadmap](docs/ROADMAP.md).

## Quick start

Requirements: CMake 3.21+, and a C compiler with C23 support (GCC 13+,
Clang 18+, or MinGW-w64 on Windows). In CLion, just open the folder.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # run the tests

./build/BulletDB data.csv                    # import data.csv, then run a query on it
```

`BulletDB` imports the CSV into `test-1.bdb` and runs the query hard-coded in
`main.c`, for example `SUM(PRICEEACH)`:

```text
+----------------+
| SUM(PRICEEACH) |
| double         |
+----------------+
|    18883385.22 |
+----------------+
AGGREGATE SUM(PRICEEACH) calls: 2  chunks: 1  rows: 1  ...
        +-SCAN (225721 rows, 5 columns, 2 groups)
```

More in [docs/USAGE.md](docs/USAGE.md): changing the query, the CSV rules,
printing chunks, and the debug dumps.

## How it works

```
 file.csv ──▶ CSV reader ──chunks──▶ writer ──▶ file.bdb
                                                   │
          result ◀── AGGREGATE ◀──chunks── SCAN ◀──┘
                     (executor)            (reader)
```

| Layer | What it does | Details |
|-------|--------------|---------|
| **Chunks** | Batches of up to 2048 rows, stored column by column with a NULL bitmap. The unit that moves through the engine. | [DESIGN.md, Part 1](docs/DESIGN.md#part-1-current-design) |
| **Storage** | `.bdb` files: a header, row groups of up to 122,880 rows (one block per column), then a footer with the schema and every block's offset | [DESIGN.md, Part 2](docs/DESIGN.md#part-2-bdb-v1-file-format) |
| **Execution** | A tree of operators passing chunks up to each other, each with `next`, `close` and `describe` | [DESIGN.md, Part 3](docs/DESIGN.md#part-3-query-execution) |

## Documentation

| Document | Contents |
|----------|----------|
| [DESIGN.md](docs/DESIGN.md) | How the engine is built and why: data model, the full `.bdb` file format spec with a byte-by-byte example, and the executor |
| [USAGE.md](docs/USAGE.md) | Running the program, changing the query, CSV rules, debug dumps, error codes |
| [TESTING.md](docs/TESTING.md) | The test suite, sanitizers, CI workflows, and how to release |
| [PERFORMANCE.md](docs/PERFORMANCE.md) | Benchmarks and the next speedups |
| [ROADMAP.md](docs/ROADMAP.md) | Progress, plans, and milestones |
| [KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) | Current limitations and known bugs |
| [CHANGELOG.md](CHANGELOG.md) | What changed in each version |
| [WRITER_FLOW.md](docs/WRITER_FLOW.md) | Step-by-step flow diagrams for the writer |

## Project layout

```
BulletDB/
├── main.c              entry point: CSV → test-1.bdb, then runs a query on it
├── src/                the engine (built as the bulletdb library)
│   ├── common/         status codes and errors
│   ├── core/           columns, chunks, the table printer
│   ├── csv/            CSV reader and tokenizer
│   ├── storage/        .bdb writer and reader, file I/O, debug dumps
│   └── executor/       operators: scan, aggregate, explain
├── tests/              CTest suite
├── docs/               design, usage, testing, performance, roadmap
└── .github/            CI, CodeQL, release workflow, Dependabot
```
