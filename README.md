# BulletDB

A columnar OLAP (analytical) database engine written in C.

BulletDB is built for analytical queries: scanning, filtering and aggregating
large tables. It stores data **by column** rather than by row, so a query that
only touches `price` and `year` never has to read any other column. Each
column's values sit next to each other in memory, which helps the CPU cache
and keeps scan loops tight.

> **Status:** early work in progress. **Milestone reached: the storage round
> trip works.** BulletDB streams a CSV as **chunks** of up to 2048 typed rows
> (INT, DOUBLE, BOOL, with NULLs), writes them to its own columnar **`.bdb`
> v1** file (row groups of column blocks, then a footer with the schema and
> the offset of every block), and **streams them back out** of that file one
> chunk at a time, holding only the footer and one chunk in memory. Next:
> checking the values end to end with `SUM`, then a tiny query engine. See
> [Current work](#current-work).

For how the engine is built and why, and for the full `.bdb` v1
specification, see **[docs/DESIGN.md](docs/DESIGN.md)**.

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

One run does a full round trip:

1. **Write:** reads the CSV one chunk at a time and writes every chunk to
   **`test-1.bdb`** in the current directory.
2. **Read back:** opens `test-1.bdb` with the `.bdb` reader and streams every
   chunk back out of it.
3. **Report:** prints how many chunks and rows came back, next to the row
   count stored in the file's footer:

```text
reader streamed 111 chunks, 225721 rows (footer says 225721)
```

For a 225,721-row file that's 60 full chunks from the first row group, 50
full chunks plus one of 441 rows from the second, and 225,721 rows in total.

To see the rows themselves, uncomment the `print_chunk_cb(chunk, &err);` line
in either loop in `main.c`. Each chunk is printed like this:

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
`src/core/chunk.h`). The last chunk holds whatever rows are left. To see
several chunks from a small CSV, lower `BDB_VECTOR_SIZE` to something like
10.

### Debug dumps

Two functions in `src/storage/debug.c` print a structured summary of a file's
layout and check it. Call them from `main` when you need them:

- `bdb_writer_debug_dump(&writer, stdout)`: after `bdb_writer_finish`, before
  `bdb_writer_close`
- `bdb_reader_debug_dump(&reader, stdout)`: after `bdb_reader_open`. Returns
  the number of problems found.

For the same file, the two should match line for line (225,721 rows, 5
columns):

```text
== BDB_WRITER ==
rows written: 225721   columns: 5   groups: 2 (capacity 8)   rows in buffer: 0
  col 0: ORDERNUMBER          type 0, 8 bytes per value
  ...
  group 0: 122880 rows
    ORDERNUMBER          offset          8 (0x00000008)  size    1105920  ok
    ...
  group 1: 102841 rows
    ORDERNUMBER          offset    5529608 (0x00546008)  size     925569  ok
    ...
  data ends at 10157453 (footer should start here)
== BDB_READER ==
rows: 225721   columns: 5   groups: 2
  col 0: ORDERNUMBER          INT     (type 0)
  ...
  group 0: 122880 rows
    ORDERNUMBER          offset          8 (0x00000008)  size    1105920  ok
    ...
  data ends at 10157453 (should equal the trailer's footer_offset)
== 0 problems ==
```

Any inconsistency in the reader's view of the file is printed as a line
starting with `!!`.

To look at the bytes themselves: `Format-Hex test-1.bdb` in PowerShell.

Exit codes:

| Code | Meaning |
|------|---------|
| `0`  | Success |
| `1`  | Bad arguments, or an import or write error |

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

- An empty field (`2024,,true`) is stored as **NULL** in every column type.
- Spaces around a value are ignored (`2023, 20.00 ,true` works).
- Each row must have exactly one value per column. A trailing comma
  (`2025,50.00,true,`) counts as an extra, empty value, so it's an error.
- Blank lines are skipped.
- Both `\n` and `\r\n` line endings work.
- A line longer than `BDB_CSV_MAX_LINE` (1 MiB) is rejected, and so is a field
  longer than `BDB_CSV_MAX_FIELD` (255 characters).

Errors name the line:

```text
error: line 7: expected 3 values, got 2
error: line 9: more values than the 3 columns in the header
error: line 12: value in column 'price' is longer than 255 characters
```

### How a line is split

`next_field` in `csv_tokenize.c` splits a line **in place**. It writes `'\0'`
over each comma, trims spaces by moving the start and end pointers, and
returns a pointer into the line buffer. Nothing is copied or allocated:

```
buffer:          "2023, 20.00 ,true"
after field 1:   "2023\0 20.00 ,true"        returns "2023"
after field 2:   "2023\0 20.00\0\0true"      returns "20.00"
after field 3:                               returns "true", then NULL
```

Two things follow from this:
- A line can only be split once. The first data row is split twice (once to
  detect types, once to store the values), so type detection works on a
  copy.
- A field pointer is valid only until the next line is read into the buffer.
  Values are converted and stored in the chunk right away, so this is fine
  for numbers and BOOL. Strings, once they exist, will have to be copied.

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
│ CSV_READER  (csv/)           │
│                              │
│   fills ──▶ ┌───────────┐    │
│             │   CHUNK   │    │  up to BDB_VECTOR_SIZE rows, reused
│             └───────────┘    │
└──────────────┬───────────────┘
               │ csv_next_chunk(&reader, &chunk, &err)
               ▼
┌──────────────────────────────┐
│ BDB_WRITER  (storage/)       │  copies chunks into a row group buffer,
│                              │  writes each full group, then the footer
└──────────────┬───────────────┘
               ▼
          test-1.bdb
```

`main` pulls chunks from the reader and hands each one to the writer, until
`csv_next_chunk` returns `NULL`. Then `bdb_writer_finish` writes the last
group, the footer and the trailer.

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

Chunk functions (`src/core/chunk.c`):

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

`.bdb` v1 is written by `BDB_WRITER` (`src/storage/bdb_writer.c`). The full
byte-level specification, with a worked 193-byte example, is in
[docs/DESIGN.md, Part 2](docs/DESIGN.md#part-2-bdb-v1-file-format). In short:

```
┌─────────────────────────────┐
│ "BDB1"  version (u32)       │  header, 8 bytes
├─────────────────────────────┤
│ Row group 0                 │  up to 122,880 rows
│   col 0: [bitmap][values]   │  one contiguous block per column
│   col 1: [bitmap][values]   │
├─────────────────────────────┤
│ Row group 1 ...             │
├─────────────────────────────┤
│ FOOTER                      │  row_count, schema (names, types),
│                             │  each group's row count and the
│                             │  offset of every column block
├─────────────────────────────┤
│ footer_offset (u64) "BDB1"  │  trailer, 12 bytes
└─────────────────────────────┘
```

- Integers are little-endian.
- A column block is `n` bitmap bytes (1 = valid, 0 = NULL) followed by `n`
  values at their real size (8 bytes for `INT` and `DOUBLE`, 1 for `BOOL`).
- The footer goes at the end because the row count and the offsets are only
  known once everything has been written. A reader starts from the last 12
  bytes, checks the magic, and jumps to the footer.
- Every column in every group has its own offset, so a reader can seek
  straight to one column without reading the others.

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
├── main.c                       entry point: CSV → chunks → test-1.bdb
├── sales.csv                    sample data
├── docs/
│   ├── DESIGN.md                how the engine is built, and the .bdb v1 spec
│   └── WRITER_FLOW.md           step-by-step flow diagrams for the writer
└── src/                         include root: #include "core/chunk.h"
    ├── common/
    │   └── common.c / .h        BdbStatus, BdbError, error helpers
    ├── core/
    │   ├── chunk.c / .h         CHUNK, chunk_reset, chunk_free, BDB_VECTOR_SIZE, BDB_ROW_GROUP_SIZE
    │   └── table.c / .h         COLUMN, TABLE, ColumnType, print_table, find, free
    ├── csv/
    │   ├── csv_reader.c / .h    CSV_READER: open, next_chunk, close
    │   └── csv_tokenize.c / .h  read_next_line, next_field, CSV limits
    ├── query/
    │   └── query.c / .h         old int64 query engine (switched off)
    └── storage/
        ├── bdb_format.h         magic, version, file limits
        ├── bdb_writer.c / .h    BDB_WRITER: open, append, finish, close
        ├── memory.c / .h        GROW_CAPACITY / GROW_ARRAY (growing arrays by doubling)
        ├── io.c / .h            checked read helper
        ├── bdb_reader.c / .h    BDB_READER: open (loads the footer), next_chunk (streams chunks), close
        └── debug.c / .h         bdb_writer_debug_dump, bdb_reader_debug_dump
```

## Current work

The engine is moving from "load the whole CSV into one `TABLE`" to "pull
chunks through the engine."

1. [x] `CHUNK`, `BDB_VECTOR_SIZE` and `BDB_ROW_GROUP_SIZE`
2. [x] `CSV_READER` as a pull-based source (`csv_open` / `csv_next_chunk` /
       `csv_close`), one pass, no row-count pass or `rewind`
3. [x] `main.c` pulls chunks from the reader
4. [x] Split CSV fields in place, and rewrite `parse_row` as a single loop
       over the columns (see [Performance](#performance))
5. [x] **`.bdb` v1 format** designed (spec in
       [docs/DESIGN.md](docs/DESIGN.md#part-2-bdb-v1-file-format)): header,
       row groups of column blocks, and a footer at the end with the schema,
       each group's row count and the offset of every column block
6. [x] **`BDB_WRITER`** (`open` / `append(chunk)` / `finish` / `close`):
       copies chunks into a row group buffer, writes each full group, then the
       last partial group, the footer and the trailer. Checked byte by byte on
       a 1-row file (193 bytes) and with 160,000 rows (2 groups)
7. [x] **`BDB_READER`** for the new format, with the same shape as
       `CSV_READER` (`open` / `next_chunk` / `close`). It keeps only the footer
       and one chunk in memory and streams the data.
       - [x] `bdb_reader_open`: header, trailer and footer (schema, group row
             counts, every block's offset). Checked with
             `bdb_reader_debug_dump`: 0 problems, and identical to the
             writer's dump on a 225,721-row, 2-group file
       - [x] `bdb_reader_next_chunk`: streams up to 2048 rows per call,
             seeking straight to each column's rows in the current group
       - [x] `bdb_reader_close`
8. [~] **Round trip:** CSV → `.bdb` → reader gives back the same rows
       - [x] Same number of chunks and rows (111 chunks, 225,721 rows)
       - [ ] Same **values**: compare `SUM` of a column computed from the CSV
             chunks and from the reader's chunks
9. [ ] **`SUM`** over a column, pulling chunks from `BDB_READER`, skipping
       NULLs: the first query on a stored file

### Milestone: storage round trip (2026-09-29)

CSV → chunks → `.bdb` → chunks works end to end. A 225,721-row, 5-column
CSV is written as 2 row groups and read back as 111 chunks with every row
accounted for, while the reader holds only the footer (a few hundred bytes)
and one 2048-row chunk in memory.

## Performance

CSV reader timings, per 2048-row chunk of a 3-column file (`INT`, `DOUBLE`,
`BOOL`), measured around each `csv_next_chunk` call:

| Version | Per chunk | Per row | Rows per second |
|---------|-----------|---------|-----------------|
| Copying each field (`extract_value`: 2 × `malloc` + `free` per field) | ~1.67 ms | ~820 ns | ~1.2 M |
| **Splitting fields in place** (`next_field`) | **~0.91 ms** | **~445 ns** | **~2.2 M** |

Splitting in place made the reader about **1.8 times faster**. It also fixed
a leak of one allocation per field.

### Full import benchmark

CSV → `.bdb`, 5 columns (4 `INT`, 1 `DOUBLE`), `-O2`, MinGW GCC, warm file
cache:

| Stage | 1 M rows | 5 M rows | Per row | Share |
|-------|----------|----------|---------|-------|
| Reading lines only (`fgets`, no parsing) | 54 ms | 272 ms | ~53 ns | baseline |
| **CSV parsing** (`csv_next_chunk`) | **580 ms** | **2,860 ms** | **~575 ns** | **~96%** |
| `SUM` of one column over each chunk | 0.4 ms | 2.3 ms | 0.45 ns | ~0% |
| Writer (`append` + `finish`) | 17 ms | 78 ms | ~16 ns | ~3% |
| **Total** | **0.6 s** | **2.9 s** | | **~1.7 M rows/s, 40 MB/s** |

- **The writer is fast** (about 60 M rows/s, close to the speed of copying
  memory), and so is a scan (about 2 billion rows/s for `SUM` on one core).
- **Almost all the time is CSV parsing**, and most of that is one function:
  **`strtod` costs about 394 ns per call** with MinGW's C library, about 68%
  of the parse time (one `DOUBLE` per row). `strtoll` costs about 10 ns, and
  a hand-written integer loop about 1.5 ns.
- `-O0` and `-O2` parse at the same speed, because the time is spent inside
  the C library, not in BulletDB's code.
- The `.bdb` file is **about 1.9 times the size of the CSV**: every integer
  takes 8 bytes, and every value has a bitmap byte. Compression and narrower
  integer types would fix this (see DESIGN.md, Later).

Possible next speedups, biggest payoff first:

1. **A fast path for decimals** instead of `strtod`: parse `39.61` as the
   integer 3961 and divide by 10², which gives exactly the same result as
   `strtod` for up to 15 significant digits and small exponents, and fall back
   to `strtod` otherwise. Expected: parsing about 2.5–3 times faster.
2. **A dedicated integer parser** instead of `strtoll`.
3. **Cheaper checks.** Have `next_field` return the field's length instead
   of calling `strlen` again for the max-field check, and check the first
   character before calling `strcasecmp` for BOOL.
4. **Reading in large blocks** (`fread` plus `memchr`) instead of calling
   `fgets` once per line. `fgets` locks the `FILE` on every call, and
   `read_next_line` scans each line with `strlen` twice.
5. Later: SIMD scanning and parsing on several threads.

To measure, time each `csv_next_chunk` call with a monotonic clock
(`QueryPerformanceCounter` on Windows, `clock_gettime(CLOCK_MONOTONIC)`
elsewhere), and turn off printing. Use a Release build and a large file, and
don't read much into the first chunk or a single slow chunk.

## Known limitations

- **Types come from the first data row only.** A later value that doesn't fit
  is stored wrongly instead of being rejected. For example, `abc` in an `INT`
  column becomes `0`.
- **No strings yet.** A `STR` column fails the import.
- **No queries yet.** Data can be written to `.bdb` and streamed back, but
  there's no query engine on top of it yet (`SUM` is next).
- **The reader always reads every column.** Reading only the columns a query
  needs (projection) is planned; the format already supports it.
- **CSV:** quoted fields and commas inside values aren't supported.
- **Fixed output file.** `main` always writes `test-1.bdb`.
- **File format:** little-endian only. No per-group statistics or compression
  yet (planned for later versions).
- **Windows only for now:** the writer and reader use `_ftelli64` and
  `_fseeki64`, which don't exist on Linux or macOS (`ftello` / `fseeko` there).
- **Blocks aren't aligned.** Values can start at odd byte offsets in the file.
  That's fine for `fread`, but reading values in place from a memory-mapped
  file would need blocks padded to 8 bytes (planned for format v2).

## Known bugs

Things that are broken right now, most serious first.

**Crashes or wrong data**
- [ ] `csv_open` ignores the status returned by `parse_header`
- [ ] An empty value in the first data row fails the import:
      `detect_file_type("")` returns `STR`, which is rejected
- [ ] A CSV with a header but no data rows writes a `.bdb` footer with 0
      columns: the writer only learns the schema from the first chunk
- [ ] `bdb_write_group` doesn't check `_ftelli64` for failure (-1) before
      storing it as an offset
- [ ] `bdb_reader_open` doesn't yet reject a bad `footer_offset`, a
      `col_count` outside 1..100, bad group row counts, or a footer that
      doesn't end at the trailer (these are only checked by
      `bdb_reader_debug_dump`). A corrupted footer could make `next_chunk`
      read from the wrong place
- [ ] `bdb_reader_open` uses plain `fseek` (32-bit on Windows) in two places
      (trailer and footer), so files over 2 GB would be read from the wrong
      position. It also doesn't check the `name_len` read or the `malloc`
      results for `row_groups` and `offsets`

**Memory**
- [ ] `free_table` doesn't free `bitmap`
- [ ] If `csv_open` fails, `main` returns without calling `csv_close`, so the
      file and buffer leak

**Cleanup**
- [ ] `chunk_init` takes a `col_count` it doesn't need (it's set again by the
      caller)
- [ ] The `memcpy` in `bdb_writer_append` adds an offset to a `void *`. GCC
      allows this as an extension, but standard C needs a `(char *)` cast
- [ ] `parse_header` still splits with `strtok`, so column names aren't
      trimmed and an empty name (`year,,price`) is skipped instead of
      reported. It could use `next_field` like the rows.

## Roadmap

- [x] CSV import with line-level error messages
- [x] Typed columns (`INT`, `DOUBLE`, `BOOL`) with NULL bitmaps
- [x] Pull-based CSV reader that streams fixed-size chunks
- [x] In-place CSV field splitting (1.8 times faster, no allocations per field)
- [x] Bordered table printer
- [x] Columnar `.bdb` v1 file writer: typed column blocks with NULL bitmaps,
      row groups, and a footer with the schema and every block's offset
- [x] `.bdb` reader: opens a file, loads its footer, and streams the data
      back one chunk at a time
- [x] Save and load tables in `.bdb`, with row groups (old int64 format, since
      replaced)
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
