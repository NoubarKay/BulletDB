# BulletDB

A columnar OLAP (analytical) database engine written in C.

BulletDB is built for analytical queries: scanning, filtering and aggregating
large tables. It stores data **by column** rather than by row, so a query that
only touches `price` and `year` never has to read any other column. Each
column's values sit next to each other in memory, which helps the CPU cache
and keeps scan loops tight.

> **Status:** early work in progress. BulletDB can import an integer CSV,
> save it to its own binary `.bdb` format in row groups, load it back, and run
> a filtered `SUM` over a column. See [Roadmap](#roadmap).

## Contents

- [Building](#building)
- [Usage](#usage)
- [CSV input](#csv-input)
- [Queries](#queries)
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

One run is an end-to-end test of the engine:

1. **Import:** loads the CSV into an in-memory table.
2. **Save:** writes the table to `test.bdb` in the current directory, in row
   groups of 2 rows.
3. **Load:** reads `test.bdb` back into a new table and checks that the row and
   column counts match the imported table.
4. **Print:** prints the loaded table.
5. **Query:** runs `SUM(price) WHERE year != 2025` on the loaded table and
   prints the result.

With the included `sales.csv`:

```text
$ BulletDB sales.csv

Wrote test.bdb
Read test.bdb: 10 rows, 2 columns
Round trip: OK
year        price
------------------------
2023        10
2024        20
2024        30
2025        40
2025        50
2025        50
2025        50
2025        50
2025        50
2025        50
(10 rows, 2 columns)
Sum of prices: 60
```

The query is currently hard-coded in `main.c`. Edit the `BdbQuery` there to try
other filters.

Exit codes:

| Code | Meaning |
|------|---------|
| `0`  | Success, and the round trip matched |
| `1`  | Bad arguments, an import, save, load or query error, or a round-trip mismatch |

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

The importer reports what went wrong and where:

```text
error: line 4: bad value 'abc' in column 'price'
error: line 7: expected 2 values, got 1
error: line 9: more values than the 2 columns in the header
```

## Queries

A query aggregates one column, optionally filtered by a condition on another
column (or the same one):

```c
BdbQuery query = {
    .agg        = BDB_AGG_SUM,
    .field      = "price",
    .has_filter = true,
    .filter     = { .field = "year", .op = BDB_OP_NE, .value = 2025 },
};

int64_t result = 0;
BdbStatus status = bdb_run_query(&table, &query, &result, &err);
```

This is equivalent to `SELECT SUM(price) FROM table WHERE year != 2025`.

Filter operators:

| Operator | Meaning |
|----------|---------|
| `BDB_OP_EQ` | `=` |
| `BDB_OP_NE` | `!=` |
| `BDB_OP_LT` | `<` |
| `BDB_OP_LE` | `<=` |
| `BDB_OP_GT` | `>` |
| `BDB_OP_GE` | `>=` |

`bdb_run_query` looks up both columns by name with `bdb_find_column`, and
returns `BDB_ERR_NOT_FOUND` if either one doesn't exist. It then makes a single
pass over the rows.

Aggregates: `BDB_AGG_SUM` works. `BDB_AGG_COUNT` is defined but not yet
implemented; the query engine currently always computes a sum.

## Architecture

```
             ┌────────────┐     ┌──────────────────┐     ┌─────────────┐
 file.csv ──▶│ CSV import │────▶│  TABLE (memory)  │────▶│ .bdb writer │──▶ file.bdb
             │  csv.c     │     │  column-oriented │     │  bdb.c      │       │
             └────────────┘     └──────────────────┘     └─────────────┘       │
                                   ▲            │                              │
                                   │            ▼                              │
                                   │     ┌──────────────┐                      │
                                   │     │ query engine │──▶ result            │
                                   │     │  query.c     │                      │
                                   │     └──────────────┘                      │
                                   │                     ┌─────────────┐       │
                                   └─────────────────────│ .bdb reader │◀──────┘
                                                         │ bdb_reader.c│
                                                         └─────────────┘
```

### In-memory table

```c
typedef struct {
    char    *name;
    int64_t *data;        // row_count values, one after another
} COLUMN;

typedef struct {
    uint64_t row_count;
    uint64_t col_count;
    uint32_t group_size;  // rows per row group when saved to .bdb
    COLUMN  *columns;
} TABLE;
```

Each column owns one flat array of values. Aggregating a column means walking
one array from start to end, which is the access pattern analytical queries
depend on.

Table helpers in `table.c`:

| Function | Purpose |
|----------|---------|
| `print_table` | Print the table with column headers |
| `bdb_find_column` | Look up a column's index by name |
| `free_table` | Free all columns and reset the table to empty |

### Importing a CSV

The importer makes two passes over the file:

1. **Count** the data rows, so each column's array can be allocated once at its
   final size.
2. **Parse** the header into columns, then parse each row and write its values
   straight into the column arrays.

This avoids growing arrays with `realloc` for every row.

## The .bdb file format

`.bdb` is BulletDB's binary table format. All integers are written in the
machine's native byte order (little-endian on x86-64 and ARM64).

A file has three parts:

```
┌──────────────────────┐
│ Header (20 bytes)    │
├──────────────────────┤
│ Column names         │
├──────────────────────┤
│ Row group 0          │
│ Row group 1          │
│ ...                  │
└──────────────────────┘
```

### 1. Header

| Offset | Size | Field | Type |
|--------|------|-------|------|
| `0x00` | 8 | `row_count`  | `uint64_t` |
| `0x08` | 8 | `col_count`  | `uint64_t` |
| `0x10` | 4 | `group_size` | `uint32_t` |

### 2. Column names

Repeated once per column:

| Size | Field | Type |
|------|-------|------|
| 4 | `name_length` | `uint32_t` |
| `name_length` | `name` | bytes, with no null terminator |

### 3. Row groups

Rows are split into groups of `group_size` rows. The last group holds whatever
is left over. Within each group, the values are stored column by column:

```
Row group 0:   col 0 [rows 0..1]   col 1 [rows 0..1]   ...
Row group 1:   col 0 [rows 2..3]   col 1 [rows 2..3]   ...
...
```

Every value is an `int64_t`.

Row groups are the unit that future features build on, such as per-group
min/max statistics for skipping groups, and per-group compression.

### Validation on load

`bdb_read` rejects a file with `BDB_ERR_FORMAT` when:
- `col_count` is 0 or greater than `BDB_MAX_COL_COUNT` (100)
- `group_size` is 0 or greater than `BDB_GROUP_SIZE` (2)

A file that ends early fails with `BDB_ERR_IO`.

The limits are defined in `src/engine/storage/format.h`.

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
| `BDB_ERR_IO` | A read or write failed or stopped early |
| `BDB_ERR_FORMAT` | The `.bdb` file is invalid or corrupted |
| `BDB_ERR_NOT_FOUND` | A column name used in a query doesn't exist |

`bdb_status_str()` returns a short generic description of any status code.

## Project layout

```
BulletDB/
├── CMakeLists.txt
├── main.c                     command-line entry point and end-to-end test
├── sales.csv                  sample data
└── src/
    ├── csv.c / csv.h          CSV importer
    └── engine/
        ├── common.c / .h      BdbStatus, BdbError, error helpers
        ├── table.c / .h       TABLE and COLUMN types, print, find, free
        ├── query/
        │   └── query.c / .h   BdbQuery, filters, bdb_run_query
        └── storage/
            ├── format.h       on-disk limits and constants
            ├── bdb.c / .h     .bdb writer (and bdb_read declaration)
            ├── bdb_reader.c   .bdb reader
            └── io.c / .h      checked read helper
```

## Known limitations

- **Integers only.** Every value is an `int64_t`. There are no floats, strings,
  dates or NULLs.
- **Queries.** `SUM` is the only working aggregate, there's one filter at most,
  and the query is hard-coded in `main.c`.
- **Row group size.** It's set to 2 in `main.c`, and the reader rejects any
  file with a larger group size.
- **File format.** There's no magic number or version field. Files use native
  byte order, so they aren't portable between little- and big-endian machines.
- **CSV.** Lines longer than 255 characters aren't handled correctly. Quoted
  fields and commas inside values aren't supported. Empty fields (`a,,b`)
  aren't detected as errors: the empty field is skipped, and the row fails
  only if it ends up with too few values.
- **Fixed output file.** The program always writes `test.bdb`.

## Roadmap

- [x] CSV import with line-level error messages
- [x] Save and load tables in `.bdb`, with row groups
- [x] `SUM` over a column
- [x] Single-condition filters (`=`, `!=`, `<`, `<=`, `>`, `>=`)
- [ ] `COUNT`, `MIN`, `MAX`, `AVG`
- [ ] Multiple filter conditions (`AND` / `OR`)
- [ ] `GROUP BY`
- [ ] Magic number and version field in the `.bdb` header
- [ ] Per-row-group min/max statistics, used to skip groups during queries
- [ ] Configurable row group size (for example, thousands of rows per group)
- [ ] More column types (`double`, strings via a dictionary)
- [ ] CSV type inference across the whole column (widen `BOOL` → `INT` → `DOUBLE` → `STR`
      during the row-count pass), with an optional user-supplied schema
- [ ] CSV import hardening:
  - [ ] Validate every value against its column type, not just the first row (`abc` in an
        `INT` column is currently stored as `0`)
  - [ ] Trim each field once and use the trimmed value for both type detection and parsing
  - [ ] Reject invalid `BOOL` values instead of storing them as `false`
  - [ ] Report an error for `STR` columns until string storage exists
  - [ ] Detect integer overflow (`strtoll` setting `ERANGE`)
  - [ ] Reject lines longer than `BDB_CSV_MAX_LINE` instead of splitting them
  - [ ] Close the file when the line buffer can't be allocated
- [ ] Column compression (run-length, delta, dictionary encoding)
- [ ] Vectorized (SIMD) scans
- [ ] Command-line subcommands such as `import`, `query` and `info`
- [ ] A small query language
