# BulletDB

A columnar OLAP (analytical) database engine written in C.

BulletDB is built for analytical queries: scanning, filtering and aggregating
large tables. It stores data **by column** rather than by row, so a query that
only touches `price` and `year` never has to read any other column. Each
column's values sit next to each other in memory, which helps the CPU cache
and keeps scan loops tight.

> **Status:** early work in progress, in the middle of a redesign. The CSV
> reader is now a **pull-based source**. You open it, ask it for the next
> **chunk** of up to 2048 typed rows (INT, DOUBLE, BOOL, with NULLs) until it
> says there are no more, then close it. `main.c` prints each chunk. The
> `.bdb` writer, the reader and the query engine still expect the old
> int64-only `TABLE`, so they're switched off until they're ported to chunks.
> See [Current work](#current-work).

## Contents

- [Building](#building)
- [Usage](#usage)
- [CSV input](#csv-input)
- [Architecture](#architecture)
- [Queries](#queries)
- [The .bdb file format](#the-bdb-file-format)
- [Error handling](#error-handling)
- [Project layout](#project-layout)
- [Current work](#current-work)
- [Performance](#performance)
- [Known limitations](#known-limitations)
- [Known bugs](#known-bugs)
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

BulletDB reads the CSV one chunk at a time and prints each chunk as a table:

```text
+------+--------+-----------+
| year | price  | expensive |
| int  | double | bool      |
+------+--------+-----------+
| 2023 |  20.00 | true      |
| 2024 |  20.00 | true      |
| 2015 |   4.99 | false     |
...
+------+--------+-----------+
(2048 rows, 3 columns)
```

A chunk holds up to `BDB_VECTOR_SIZE` rows (2048, set in
`src/engine/storage/format.h`). The last chunk holds whatever rows are left.
To see several chunks from the small `sales.csv`, lower `BDB_VECTOR_SIZE` to
something like 10.

Exit codes:

| Code | Meaning |
|------|---------|
| `0`  | Success |
| `1`  | Bad arguments or an import error |

Relative paths are resolved from the current working directory. In CLion, set
**Run → Edit Configurations… → Working directory** to `$ProjectFileDir$` and
put the CSV name in **Program arguments**.

## CSV input

```csv
year,price,expensive
2023,20.00,true
2024,,true
```

Rules:
- The first line is the header and holds the column names.
- Each column's type is detected from its value in the **first data row**:

  | Value looks like | Type | Stored as |
  |------------------|------|-----------|
  | `true` `false` `t` `f` `yes` `no` (any case) | `BOOL` | `bool` |
  | A whole number, such as `2023` | `INT` | `int64_t` |
  | A decimal number, such as `20.00` | `DOUBLE` | `double` |
  | Anything else | `STR` | not supported yet, so the import fails |

- An empty field (`2024,,true`) is stored as **NULL** for `INT` and `DOUBLE`
  columns.
- Each row must have one value per column.
- Blank lines are skipped.
- Both `\n` and `\r\n` line endings work.
- A line longer than `BDB_CSV_MAX_LINE` (1 MiB) is rejected, and so is a field
  longer than `BDB_CSV_MAX_FIELD` (255 characters).

Errors name the line:

```text
error: line 9: more values than the 3 columns in the header
error: line 12: value in column 'price' is longer than 255 characters
```

## Architecture

The design is modeled loosely on DuckDB. Data moves through the engine in
**chunks** (vectors) of rows, and is stored on disk in larger **row groups**.
Sources are **pull-based**: the caller asks for the next chunk, and the
source fills it and hands it back.

```
 file.csv
    │  one line at a time (read_next_line)
    ▼
┌──────────────────────────────┐
│ CSV_READER  (csv.c)          │
│                              │
│   fills ──▶ ┌───────────┐    │
│             │   CHUNK   │    │  up to BDB_VECTOR_SIZE rows, reused
│             └───────────┘    │
└──────────────┬───────────────┘
               │ csv_next_chunk(&reader, &chunk, &err)
               ▼
      ┌──────────────────┐
      │ main.c loop      │   today: print the chunk
      │                  │   planned: BDB_WRITER appends it to a row group
      └──────────────────┘
               │
               └──▶ ask for the next chunk, until chunk == NULL
```

The reader keeps only **one chunk** in memory, whatever the file's size. Each
call to `csv_next_chunk` clears it and fills it again.

| | Size | Purpose |
|---|---|---|
| Chunk (vector) | `BDB_VECTOR_SIZE` = 2048 rows | The unit that moves through the engine. Small enough to stay in CPU cache. |
| Row group | `BDB_ROW_GROUP_SIZE` = 60 chunks = 122,880 rows | The unit stored on disk, and later compressed and given min/max statistics. |

A row group is always a whole number of chunks, so a chunk never spans two
row groups.

### Columns and chunks

```c
typedef struct {
    char           *name;
    enum ColumnType type;    // BDB_COL_INT, BDB_COL_DOUBLE, BDB_COL_BOOL
    void           *data;    // typed array: int64_t*, double* or bool*
    char           *bitmap;  // one byte per row: 1 = valid, 0 = NULL
} COLUMN;

typedef struct {
    uint64_t count;          // rows filled so far, 0..BDB_VECTOR_SIZE
    uint64_t col_count;
    COLUMN  *columns;
} CHUNK;
```

Each column's `data` and `bitmap` arrays are allocated **once**, with room for
`BDB_VECTOR_SIZE` rows, as soon as the first data row has fixed the types.
They're reused for every chunk. Only the first `count` entries hold real rows.

```
CHUNK  count = 3
┌──────────────────┬──────────────────┬──────────────────┐
│ year    (int)    │ price   (double) │ expensive (bool) │
│ data:   2023     │ data:   20.00    │ data:   true     │
│         2024     │         0.00     │         true     │
│         2025     │         40.00    │         false    │
│ bitmap: 1 1 1    │ bitmap: 1 0 1    │ bitmap: 1 1 1    │
└──────────────────┴──────────────────┴──────────────────┘
                   0 in the bitmap = NULL
```

Chunk functions (`src/chunk.c`):

| Function | Purpose |
|----------|---------|
| `chunk_reset` | Set `count` to 0 so the chunk can be filled again |
| `chunk_free` | Free each column's `name`, `data` and `bitmap`, then the column array. Safe to call twice. |

### The CSV reader

```c
typedef struct {
    FILE    *file;
    char    *buffer;       // one line, up to BDB_CSV_MAX_LINE bytes
    uint64_t line_no;
    bool     have_types;   // set once the first data row has fixed the types
    CHUNK    chunk;        // reused for every chunk
} CSV_READER;

BdbStatus csv_open(CSV_READER *reader, const char *path, BdbError *err);
BdbStatus csv_next_chunk(CSV_READER *reader, const CHUNK **out, BdbError *err);
void      csv_close(CSV_READER *reader);
```

- **`csv_open`** opens the file, allocates the line buffer and parses the
  header into the chunk's column names.
- **`csv_next_chunk`** reads lines until the chunk is full or the file ends.
  On the first data row it detects the types and allocates the column
  buffers. It sets `*out` to the chunk, or to `NULL` when there are no rows
  left.
- **`csv_close`** frees the chunk and the buffer and closes the file. The
  caller owns the `CSV_READER` struct itself, which is usually on the stack.

The whole import is one loop:

```c
CSV_READER reader = {0};
const CHUNK *chunk;

BdbStatus status = csv_open(&reader, path, &err);
while (status == BDB_OK &&
       (status = csv_next_chunk(&reader, &chunk, &err)) == BDB_OK && chunk != NULL) {
    /* use chunk */
}
csv_close(&reader);
```

The returned chunk belongs to the reader and is overwritten by the next call,
so a consumer must copy anything it wants to keep.

### The table printer

`print_table` in `table.c` sizes each column to its longest name, type or
value. It right-aligns numbers, prints `NULL` for invalid rows and `true` or
`false` for BOOL values, and shows doubles with two decimals. A table longer
than 20 rows shows only the first and last 10 rows. A column whose `bitmap` is
`NULL` is treated as having no NULLs.

`print_chunk_cb` in `main.c` prints a chunk by wrapping it in a `TABLE` that
points at the same columns, so nothing is copied.

## Queries

> Switched off during the move to chunks. The scan loop in `query.c` is
> commented out, so `bdb_run_query` returns 0.

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
returns `BDB_ERR_NOT_FOUND` if either one doesn't exist.

## The .bdb file format

> This describes the **old, int64-only** format that `bdb.c` and
> `bdb_reader.c` still implement. Neither is wired up at the moment. The
> planned replacement is described under [Current work](#current-work).

All integers are written in the machine's native byte order (little-endian on
x86-64 and ARM64).

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

**Header**

| Offset | Size | Field | Type |
|--------|------|-------|------|
| `0x00` | 8 | `row_count`  | `uint64_t` |
| `0x08` | 8 | `col_count`  | `uint64_t` |
| `0x10` | 4 | `group_size` | `uint32_t` |

**Column names**, repeated once per column:

| Size | Field | Type |
|------|-------|------|
| 4 | `name_length` | `uint32_t` |
| `name_length` | `name` | bytes, with no null terminator |

**Row groups:** rows are split into groups of `group_size` rows, and the last
group holds whatever is left over. Within a group, the values are stored
column by column, and every value is an `int64_t`:

```
Row group 0:   col 0 [rows 0..1]   col 1 [rows 0..1]   ...
Row group 1:   col 0 [rows 2..3]   col 1 [rows 2..3]   ...
```

`bdb_read` rejects a file with `BDB_ERR_FORMAT` when `col_count` is 0 or
greater than `BDB_MAX_COL_COUNT` (100), or `group_size` is 0 or greater than
`BDB_GROUP_SIZE` (2). A file that ends early fails with `BDB_ERR_IO`.

## Error handling

Functions that can fail return a `BdbStatus` and fill in a `BdbError`
containing a readable message:

```c
BdbError err = {0};
BdbStatus status = csv_open(&reader, "sales.csv", &err);
if (status != BDB_OK) {
    fprintf(stderr, "error: %s\n", err.message);
}
```

| Status | Meaning |
|--------|---------|
| `BDB_OK` | Success |
| `BDB_ERR_OPEN` | A file couldn't be opened |
| `BDB_ERR_NOMEM` | A memory allocation failed |
| `BDB_ERR_PARSE` | The CSV has a bad value, the wrong number of fields, or a line or field that's too long |
| `BDB_ERR_IO` | A read or write failed or stopped early |
| `BDB_ERR_FORMAT` | The `.bdb` file is invalid or corrupted |
| `BDB_ERR_NOT_FOUND` | A column name used in a query doesn't exist |
| `BDB_ERR_INVALID` | An invalid argument, such as an unknown aggregate |

`bdb_status_str()` returns a short generic description of any status code.

## Project layout

```
BulletDB/
├── CMakeLists.txt
├── main.c                       entry point: pulls chunks from the CSV reader and prints them
├── sales.csv                    sample data
└── src/
    ├── csv.c / csv.h            CSV_READER: open, next_chunk, close
    ├── chunk.c                  chunk_init, chunk_reset, chunk_free
    └── engine/
        ├── common.c / .h        BdbStatus, BdbError, error helpers
        ├── table.c / .h         COLUMN, TABLE, ColumnType, print_table, find, free
        ├── chunk.h              CHUNK and its functions
        ├── csv/
        │   └── csv_tokenize.c/.h  read_next_line, extract_value
        ├── sink/
        │   └── bdb_sink.h       BdbChunkFn (from the earlier push design, now unused)
        ├── query/
        │   └── query.c / .h     BdbQuery, filters, bdb_run_query
        └── storage/
            ├── format.h         limits and sizes (BDB_VECTOR_SIZE, BDB_ROW_GROUP_SIZE, ...)
            ├── bdb.c / .h       old .bdb writer (and bdb_read declaration)
            ├── bdb_reader.c     old .bdb reader
            └── io.c / .h        checked read helper
```

## Current work

The engine is moving from "load the whole CSV into one `TABLE`" to "pull
chunks through the engine."

1. [x] `CHUNK`, `BDB_VECTOR_SIZE` and `BDB_ROW_GROUP_SIZE`
2. [x] `CSV_READER` as a pull-based source (`csv_open` / `csv_next_chunk` /
       `csv_close`), one pass, no row-count pass or `rewind`
3. [x] `main.c` pulls chunks and prints them
4. [ ] Clean up the CSV reader (see [Known bugs](#known-bugs)), and split
       fields in place (see [Performance](#performance))
5. [ ] **New `.bdb` format**: magic number and version, a type byte for each
       column, and for each row group each column's values at their real size
       plus its bitmap. It ends with a **footer** that holds `row_count`,
       column names and types, and the byte offset of each row group, with
       room for min/max statistics for each group. The footer goes at the end
       because `row_count` isn't known until the last chunk has been written.
6. [ ] **`BDB_WRITER`** (`open` / `append(chunk)` / `finish` / `close`): it
       copies each chunk into a row group buffer, writes the group once it
       holds `BDB_ROW_GROUP_SIZE` rows, and at the end writes the last,
       partial group and the footer. Memory use stays fixed no matter how big
       the CSV is.
7. [ ] **`BDB_READER`** for the new format, with the same shape as
       `CSV_READER` (`open` / `next_chunk` / `close`)
8. [ ] **Query engine** that pulls chunks from `BDB_READER`, with types and
       NULL handling

## Performance

A first measurement of the CSV reader, before any tuning, was about **1.7 ms
per 2048-row chunk** (3 columns), or roughly 800 ns per row. That's around
1.2 million rows per second, or about 25 MB/s. Planned improvements, biggest
payoff first:

1. **Split fields in place.** `extract_value` currently makes two `malloc`
   calls and one `free` for every field. Writing `'\0'` over each comma and
   returning a pointer into the line buffer removes all of them.
2. **Cheaper checks.** `token[0] == '\0'` instead of `strcmp(token, "")`, no
   repeated `strlen`, and checking the first character before `strcasecmp`.
3. **A dedicated integer parser** instead of `strtoll`.
4. **Reading in large blocks** (`fread` plus `memchr`) instead of calling
   `fgets` once per line.
5. Later: SIMD scanning and parsing on several threads.

Measure with a Release build (`-O2`) and a large file, and ignore the first
chunk, which also pays for type detection and allocation.

## Known limitations

- **Types come from the first data row only.** A later value that doesn't fit
  is stored wrongly instead of being rejected. For example, `abc` in an `INT`
  column becomes `0`.
- **No strings yet.** A `STR` column fails the import.
- **No saving, loading or queries** until they're ported to chunks.
- **CSV:** quoted fields and commas inside values aren't supported.
- **File format:** there's no magic number or version field, and files use
  native byte order.

## Known bugs

Things that are broken right now, most serious first.

**Crashes or wrong data**
- [ ] `csv_open` counts lines in a local `line_no` instead of
      `reader->line_no`, so after the header the count restarts at 0 and
      every error message names the line before the real one
- [ ] `csv_open` ignores the status returned by `parse_header`
- [ ] An empty value in the first data row fails the import:
      `detect_file_type("")` returns `STR`, which is rejected
- [ ] A first data row with fewer values than the header leaves the missing
      columns unallocated, so the next row writes through a `NULL` pointer
- [ ] A trailing comma (`2025,50.00,true,`) sets that row's first value to
      NULL: the extra empty token makes the outer `while` in `parse_row` run
      again
- [ ] An empty `BOOL` value is stored as `false` and marked valid instead of
      NULL

**Memory**
- [ ] The tokens returned by `extract_value` are never freed by
      `detect_types_allo_chunk_buffers` or `parse_row`, so every field leaks
      one allocation. `destination2` also isn't checked for `NULL`. Splitting
      fields in place fixes both.
- [ ] The `bitmap` `calloc` in `detect_types_allo_chunk_buffers` isn't checked
      for `NULL`
- [ ] `free_table` doesn't free `bitmap`
- [ ] If `csv_open` fails, `main` returns without calling `csv_close`, so the
      file and buffer leak

**Cleanup**
- [ ] `bdb_sink.h` (`BdbChunkFn`) and the `ctx` parameter of `print_chunk_cb`
      are left over from the earlier push design
- [ ] `chunk_init` takes a `col_count` and an `err` that it doesn't need

## Roadmap

- [x] CSV import with line-level error messages
- [x] Typed columns (`INT`, `DOUBLE`, `BOOL`) with NULL bitmaps
- [x] Pull-based CSV reader that streams fixed-size chunks
- [x] Bordered table printer
- [x] Save and load tables in `.bdb`, with row groups (old int64 format)
- [x] `SUM` over a column, with single-condition filters (old int64 engine)
- [ ] Everything under [Current work](#current-work)
- [ ] CSV reader speedups (see [Performance](#performance))
- [ ] `COUNT`, `MIN`, `MAX`, `AVG`
- [ ] Multiple filter conditions (`AND` / `OR`)
- [ ] `GROUP BY`
- [ ] Per-row-group min/max statistics, used to skip groups during queries
- [ ] Vectorized execution: queries run over one chunk at a time, in loops
      the compiler can turn into SIMD instructions
- [ ] Pack NULL bitmaps into 1 bit per row, stored as `uint64_t` words
      (32 words per 2048-row chunk)
- [ ] Strings, stored with a dictionary
- [ ] CSV type inference over the whole first chunk instead of one row, with
      an optional user-supplied schema
- [ ] CSV import hardening:
  - [ ] Validate every value against its column type
  - [ ] Reject invalid `BOOL` values instead of storing them as `false`
  - [ ] Detect integer overflow (`strtoll` setting `ERANGE`)
- [ ] Column compression (run-length, delta, dictionary encoding)
- [ ] Command-line subcommands such as `import`, `query` and `info`
- [ ] A small query language
