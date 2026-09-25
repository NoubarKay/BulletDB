# BulletDB

An OLAP (analytical) database engine written in C.

BulletDB is built for analytical queries: scanning, filtering and aggregating
large tables. It stores data **by column** rather than by row, so a query that
only touches `price` never has to load `year`. Each column's values sit next to
each other in memory, which helps the CPU cache and makes tight loops (and
later, SIMD) effective.

> **Status:** early work in progress. BulletDB can load integer CSV files into
> an in-memory column store, print them, and save the table's shape to a binary
> `.bdb` file. It doesn't run queries yet. See [Roadmap](#roadmap).

## Contents

- [Building](#building)
- [Usage](#usage)
- [CSV input](#csv-input)
- [Architecture](#architecture)
- [The .bdb file format](#the-bdb-file-format)
- [Error handling](#error-handling)
- [Project layout](#project-layout)
- [Known limitations](#known-limitations)
- [Roadmap](#roadmap)

## Building

Requirements:
- CMake 4.3 or newer (CLion ships with a copy)
- A C compiler such as GCC or Clang (MinGW on Windows)

In CLion, open the project folder and build the `BulletDB` target.

From the command line:

```sh
cmake -B cmake-build-debug
cmake --build cmake-build-debug
```

The executable is built at `cmake-build-debug/BulletDB` (`BulletDB.exe` on
Windows).

## Usage

```sh
BulletDB <file.csv>
```

One run does the following:

1. Loads the CSV into an in-memory table.
2. Prints the table.
3. Writes the table to `test.bdb` in the current directory.
4. Reads `test.bdb` back and checks that the row and column counts match what
   was loaded.

Example with a 3-row CSV:

```text
$ BulletDB sales.csv
year        price
------------------------
2023        10
2024        20
2024        30
(3 rows, 2 columns)

Wrote test.bdb
Read test.bdb: 3 rows, 2 columns
Round trip: OK
```

Exit codes:

| Code | Meaning |
|------|---------|
| `0`  | Success, and the round trip matched |
| `1`  | Bad arguments, a load or save error, or a round-trip mismatch |

Relative paths are resolved from the current working directory. In CLion, set
**Run → Edit Configurations… → Working directory** to `$ProjectFileDir$` and
put the CSV name in **Program arguments**.

## CSV input

```csv
year,price
2023,10
2024,20
```

Rules:
- The first line is the header and holds the column names.
- Every value must be a whole number that fits in a 64-bit signed integer
  (`int64_t`).
- Each row must have exactly one value per column.
- Blank lines are skipped.
- Both `\n` and `\r\n` line endings work.

The loader reports what went wrong and where:

```text
error: line 4: bad value 'abc' in column 'price'
error: line 7: expected 2 values, got 1
error: line 9: more values than the 2 columns in the header
```

## Architecture

```
             ┌───────────┐      ┌──────────────────┐      ┌────────────┐
 file.csv ──▶│ CSV loader│─────▶│  TABLE (memory)  │─────▶│ .bdb writer│──▶ file.bdb
             │  csv.c    │      │  column-oriented │      │  bdb.c     │
             └───────────┘      └──────────────────┘      └────────────┘
                                         ▲                       │
                                         └──── .bdb reader ◀─────┘
```

### In-memory table

```c
typedef struct {
    char    *name;
    int64_t *data;      // row_count values, stored one after another
} COLUMN;

typedef struct {
    uint64_t row_count;
    uint64_t col_count;
    COLUMN  *columns;
} TABLE;
```

Each column owns one flat array of values. To sum a column you walk a single
array from start to end. That's the access pattern analytical queries depend
on.

All counts and indexes use `uint64_t`, which is also the width used on disk.

### Loading a CSV

Loading takes two passes over the file:

1. **Count** the data rows, so each column's array can be allocated once at its
   final size.
2. **Parse** the header into columns, then parse each row and write its values
   straight into the column arrays.

This avoids growing arrays with `realloc` for every row.

## The .bdb file format

`.bdb` is BulletDB's binary table format. All integers are written in the
machine's native byte order (little-endian on x86-64 and ARM64).

Current layout:

| Field | Type | Notes |
|-------|------|-------|
| `row_count` | `uint64_t` | Number of rows |
| `col_count` | `uint64_t` | Number of columns |
| *repeated `col_count` times:* | | |
| `name_length` | `int64_t` | Length of the column name in bytes |
| `name` | `char[name_length]` | Column name, with no null terminator |

Column **data is not written yet**, and the reader loads only `row_count` and
`col_count`. The format has no magic number or version field yet, so it will
change.

## Error handling

Functions that can fail return a `BdbStatus` and fill in a `BdbError`
containing a readable message:

```c
BdbError err = {0};
BdbStatus status = read_csv("sales.csv", &table, &err);
if (status != BDB_OK) {
    fprintf(stderr, "error: %s\n", err.message);
}
```

| Status | Meaning |
|--------|---------|
| `BDB_OK` | Success |
| `BDB_ERR_OPEN` | A file couldn't be opened |
| `BDB_ERR_NOMEM` | A memory allocation failed |
| `BDB_ERR_PARSE` | The CSV has a bad value or the wrong number of fields |
| `BDB_ERR_IO` | A read or write failed partway through |
| `BDB_ERR_FORMAT` | The `.bdb` file is invalid or corrupted |
| `BDB_ERR_NOT_FOUND` | Something that was looked up, such as a column, doesn't exist |

`bdb_status_str()` returns a short generic description of any status code.

## Project layout

```
BulletDB/
├── CMakeLists.txt
├── main.c                 command-line entry point
└── src/
    ├── csv.c / csv.h      CSV loader
    └── engine/
        ├── table.c / .h   TABLE and COLUMN types, print_table, free_table
        ├── bdb.c / .h     .bdb read and write
        └── common.c / .h  BdbStatus, BdbError, error helpers
```

## Known limitations

- **Integers only.** Every value is an `int64_t`. There are no floats, strings,
  dates or NULLs.
- **Line length.** Lines longer than 255 characters aren't handled correctly.
- **Simple CSV only.** There's no support for quoted fields or commas inside
  values. Empty fields (`a,,b`) aren't detected as errors either: the empty
  field is skipped, and the row fails only if it ends up with too few values.
- **`.bdb` doesn't store data yet.** See [the file format](#the-bdb-file-format).
- **Fixed output file.** The program always writes `test.bdb`.
- **Memory.** `free_table` doesn't yet free the `columns` array itself.

## Roadmap

Planned, roughly in order:

- [ ] Save and load column data in `.bdb`, with a magic number and version
- [ ] Aggregations over columns: `COUNT`, `SUM`, `MIN`, `MAX`, `AVG`
- [ ] Filters, e.g. `WHERE year = 2024`
- [ ] `GROUP BY`
- [ ] More column types (`double`, strings via a dictionary)
- [ ] Column compression (run-length, delta, dictionary encoding)
- [ ] Vectorized (SIMD) scans
- [ ] Command-line subcommands such as `import`, `query` and `info`
- [ ] A small query language
