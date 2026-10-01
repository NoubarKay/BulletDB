# BulletDB design

This document describes how BulletDB is built today: the data model, the
`.bdb` v1 file format with its writer and reader, and the query executor.

- Part 1, [Current design](#part-1-current-design), covers the data model and
  the CSV side.
- Part 2, [.bdb v1](#part-2-bdb-v1-file-format), covers the file format. The
  **writer and reader are built**, and the storage round trip (CSV → `.bdb` →
  chunks) gives back every value exactly.
- Part 3, [Query execution](#part-3-query-execution), covers the executor: a
  tree of operators passing chunks to each other. **`SCAN`, `FILTER` (with
  selection vectors) and `AGGREGATE` (`SUM`, `COUNT`, `MIN`, `MAX`, `AVG`)
  work**, together with a plan printer and per-operator stats and timing
  (D1–D3). Per-group statistics and zone-map pruning (format v2) are next.

This document covers the structure of the engine and the reasons behind it.
Running it is in [USAGE.md](USAGE.md), tests and CI in
[TESTING.md](TESTING.md), and known bugs in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Contents

- [Goals](#goals)
- [Part 1: Current design](#part-1-current-design)
  - [Overview](#overview)
  - [Data model](#data-model)
  - [Chunks and row groups](#chunks-and-row-groups)
  - [The pull model](#the-pull-model)
  - [CSV reader](#csv-reader)
  - [Memory ownership](#memory-ownership)
  - [Error handling](#error-handling)
  - [What's switched off](#whats-switched-off)
- [Part 2: .bdb v1 file format](#part-2-bdb-v1-file-format)
  - [Requirements](#requirements)
  - [File layout](#file-layout)
  - [Row groups](#row-groups)
  - [Footer](#footer)
  - [Reading a file](#reading-a-file)
  - [Worked example](#worked-example)
  - [Validation](#validation)
  - [Writer](#writer)
  - [Reader](#reader)
  - [Decisions](#decisions)
- [Part 3: Query execution](#part-3-query-execution)
  - [The model: Volcano with vectors](#the-model-volcano-with-vectors)
  - [The base operator](#the-base-operator)
  - [Scan](#scan)
  - [Aggregate](#aggregate)
  - [Filter](#filter)
  - [Streaming operators and pipeline breakers](#streaming-operators-and-pipeline-breakers)
  - [Explain and the execution debugger](#explain-and-the-execution-debugger)
  - [Next steps](#next-steps)
  - [Execution decisions](#execution-decisions)
- [Later](#later)

---

## Goals

BulletDB is a columnar OLAP engine: it's built for **scanning, filtering and
aggregating** large tables, not for updating single rows.

That drives three choices throughout the design:

1. **Store by column.** A query that reads `price` should never have to read
   `year`. Each column's values sit together, in memory and on disk.
2. **Move data in batches.** Work happens on a *chunk* of rows at a time,
   never row by row, so loops are tight and stay in the CPU cache.
3. **Fixed memory for any file size.** Importing or scanning a file bigger
   than RAM must work, so nothing ever holds a whole table in memory.

The overall shape is modeled loosely on DuckDB (chunks moving through the
engine) and Parquet (row groups plus a footer on disk).

---

# Part 1: Current design

## Overview

```
 file.csv
    │  read_next_line: one line at a time
    ▼
┌──────────────────────────────────┐
│ CSV_READER  (csv/csv_reader.c)   │
│   next_field splits a line       │
│   parse_row fills ──▶ CHUNK      │   up to BDB_VECTOR_SIZE rows, reused
└──────────────┬───────────────────┘
               │ csv_next_chunk(&reader, &chunk, &err)
               ▼
┌──────────────────────────────────┐
│ BDB_WRITER  (storage/bdb_writer.c)│
│   copies chunks into a row group │
│   writes groups, footer, trailer │──▶ file.bdb   (see Part 2)
└──────────────────────────────────┘
```

All paths are under `src/`, which is the include root: code writes
`#include "core/chunk.h"`.

| Layer | Files | Job |
|-------|-------|-----|
| Common | `common/common.c` | `BdbStatus`, `BdbError` |
| Core | `core/chunk.c`, `core/table.c` | `COLUMN`, `CHUNK`, `TABLE`, types, `print_table`. `chunk.h` defines `BDB_VECTOR_SIZE` and `BDB_ROW_GROUP_SIZE` |
| CSV | `csv/csv_tokenize.c`, `csv/csv_sniffer.c`, `csv/csv_reader.c` | Read a line, split fields (`next_field`); sniff column types from a sample (`csv_sniff`); header, strict value parsing, lines → chunks. `csv_tokenize.h` holds the CSV limits, `csv_sniffer.h` the sample size |
| Storage | `storage/bdb_writer.c`, `storage/bdb_reader.c`, `storage/bdb_format.h` | Chunks → `.bdb` file, and `.bdb` file → chunks again (footer loaded once, data streamed). `bdb_format.h` holds the magic, version and file limits |
| Storage helpers | `storage/memory.c`, `storage/io.c`, `storage/debug.c`, `storage/io.h` | `GROW_CAPACITY` / `GROW_ARRAY`, a checked `fread` helper, `bdb_writer_debug_dump` / `bdb_reader_debug_dump`. `io.h` also provides `bdb_fseek`, `bdb_ftell` (64-bit seek/tell, wrapping `_fseeki64`/`_ftelli64` on Windows and `fseeko`/`ftello` on POSIX) and `bdb_now` (monotonic nanosecond clock, wrapping `QueryPerformanceCounter` on Windows and `clock_gettime(CLOCK_MONOTONIC)` on POSIX) |
| Executor | `executor/bdb_operator.h`, `executor/scan.c`, `executor/aggregate.c`, `executor/explain.c` | The operator tree that runs queries on chunks (Part 3) |

## Data model

### Types

```c
enum ColumnType {
    BDB_COL_INT    = 0,   // int64_t, 8 bytes
    BDB_COL_DOUBLE = 1,   // double,  8 bytes
    BDB_COL_BOOL   = 3,   // bool,    1 byte
    BDB_COL_STR    = 4    // detected, but not supported yet
};
```

These numbers will be **stored in files**, so they must never change. New
types get new numbers. (2 is unused.)

`bdb_col_type_size(type)` returns the size of one value.

### Columns

```c
typedef struct {
    char           *name;
    enum ColumnType type;
    void           *data;    // typed array: int64_t*, double* or bool*
    char           *bitmap;  // one byte per row: 1 = valid, 0 = NULL
} COLUMN;
```

- `data` holds the values back to back. It's `void *` because the type is
  only known at runtime. Code casts it according to `type`.
- `bitmap` is the **validity mask**: `1` means the row has a value, `0`
  means it's NULL. A NULL row still has a slot in `data`, holding 0.
- A `NULL` `bitmap` pointer means "no NULLs in this column."
  `print_table` relies on this.

## Chunks and row groups

```c
typedef struct {
    uint64_t count;       // rows filled, 0..BDB_VECTOR_SIZE
    uint64_t col_count;
    COLUMN  *columns;
    const uint16_t *sel_vector;   // NULL = rows 0..count-1; see "Selection vectors"
} CHUNK;
```

| | Size | Where it lives | Purpose |
|---|---|---|---|
| **Chunk** (vector) | `BDB_VECTOR_SIZE` = 2048 rows | Memory | The unit that moves between parts of the engine. 2048 × 8 bytes = 16 KB per column, small enough to stay in cache. |
| **Row group** | `BDB_ROW_GROUP_SIZE` = 60 chunks = 122,880 rows | Disk | The unit that's stored, and later compressed and given statistics. Big enough for those to pay off. |

A row group is always a **whole number of chunks**, so a chunk never spans
two row groups.

A chunk's column arrays are allocated **once**, with room for
`BDB_VECTOR_SIZE` rows, and reused for every chunk. Only the first `count`
entries are real. `chunk_reset` sets `count` to 0. It doesn't clear the
arrays, because every row that's written sets both its value and its bitmap
byte.

```
CHUNK  count = 3            (capacity 2048)
┌──────────────────┬──────────────────┬──────────────────┐
│ year    (int)    │ price   (double) │ expensive (bool) │
│ data:   2023     │ data:   20.00    │ data:   true     │
│         2024     │         0.00     │         true     │
│         2025     │         40.00    │         false    │
│         ...      │         ...      │         ...      │  ← stale, ignored
│ bitmap: 1 1 1    │ bitmap: 1 0 1    │ bitmap: 1 1 1    │
└──────────────────┴──────────────────┴──────────────────┘
```

### Selection vectors

A chunk can point at **some** of its rows instead of all of them. When
`sel_vector` is `NULL`, the chunk's rows are `0..count-1`. Otherwise
`sel_vector[0..count-1]` holds the **physical** row numbers that belong to the
chunk, and `count` is how many there are:

```
columns (shared, not copied):   qty: 10  20  NULL  40  50  60
                                     0   1   2     3   4   5
FILTER qty >= 40 returns:       count = 3, sel_vector = {3, 4, 5}
```

This is how `FILTER` avoids copying (see [Filter](#filter)). The rule for
every consumer is the same: display or loop index `i`, physical row
`k = sel_vector ? sel_vector[i] : i`, and read `data[k]` and `bitmap[k]`.

| Consumer | With a selection vector |
|----------|-------------------------|
| `AGGREGATE` | follows it |
| `FILTER` | follows its input's, and writes physical rows into its own |
| `print_table` | follows it (`TABLE.sel_vector`, set by `print_chunk_cb`) |
| `bdb_writer_append` | **rejects** the chunk with `BDB_ERR_INVALID`: it copies rows `0..count-1` as one block, so it would save the wrong rows |

`sel_vector` belongs to whoever produced the chunk and, like the chunk, is
only valid until that producer's next `next` call. `chunk_reset` and
`chunk_free` set it back to `NULL`.

## The pull model

Sources are **pull-based**: the consumer asks for the next chunk, and the
source fills one and returns it.

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

This was chosen over a push design (the source calling a callback for each
chunk) because:

- **The consumer controls the loop.** It can stop, report an error, or
  combine two sources, without callbacks or `void *ctx` plumbing.
- **Cleanup is simple.** Each source has one `close`, and the caller calls it
  on every path.
- **Every source can have the same shape.** The planned `.bdb` reader uses
  the same `open` / `next_chunk` / `close` API, so the query engine will pull
  from a file exactly as `main` pulls from a CSV.

The rule that comes with it: **the returned chunk belongs to the source** and
is overwritten by the next `next_chunk` call. A consumer that wants to keep
data must copy it.

## CSV reader

```c
typedef struct {
    FILE    *file;
    char    *buffer;       // one line, up to BDB_CSV_MAX_LINE bytes
    uint64_t line_no;      // for error messages
    CHUNK    chunk;        // reused for every chunk
} CSV_READER;
```

### Lifecycle

```
csv_open
  ├─ fopen, allocate the line buffer
  ├─ read line 1 → parse_header → one COLUMN per name (data and bitmap NULL)
  ├─ remember the position and line number after the header
  ├─ csv_sniff               → read up to BDB_CSV_SAMPLE_ROWS rows, set each column's type
  ├─ allocate data and bitmap for each column (2048 slots, sized by its type)
  └─ seek back to the first data row, restore line_no

csv_next_chunk  (called repeatedly)
  ├─ chunk_reset
  └─ loop until the chunk is full or the file ends:
       read_next_line          (EOF → stop)
       blank line?             → skip
       parse_row               → check and store values at [chunk.count]
       chunk.count++
  → *out = chunk, or NULL if no rows were read

csv_close
  └─ chunk_free, fclose, free the buffer
```

### Splitting fields

`next_field` splits a line **in place**. It writes `'\0'` over each comma,
trims spaces by moving pointers, and returns a pointer into the line buffer:

```
buffer:          "2023, 20.00 ,true"
after field 1:   "2023\0 20.00 ,true"        returns "2023"
after field 2:   "2023\0 20.00\0\0true"      returns "20.00"
after field 3:                               returns "true", then NULL
```

No field is copied or allocated. That made the reader about 1.8 times faster
than the earlier version, which copied every field (see
[PERFORMANCE.md](PERFORMANCE.md)). Consequences:

- A line can only be split once. The sniffer and the reader each read the
  sampled lines themselves (the reader after seeking back), so no line is
  ever copied.
- A field pointer is only valid until the next line is read. Numbers and BOOL
  values are converted immediately. Strings, once they exist, must be copied.

### Type detection

Types are decided **before** any row is stored, in two steps: the sniffer
picks one type per column from a sample, and the reader then enforces it on
every row. This is the same split DuckDB's CSV reader uses.

**1. Sniff (`csv_sniff`).** The sniffer reads up to `BDB_CSV_SAMPLE_ROWS`
(30,720) data rows and keeps one state per column. Each non-empty value is
classified on its own:

| Value | Kind |
|-------|------|
| `true` `false` `t` `f` `yes` `no` (any case) | `BOOL`, and a *bool word* |
| `0` or `1` | `BOOL` (could also be an `INT`) |
| Parses fully as an `int64_t`, no overflow | `INT` |
| Parses fully as a decimal (including integers too big for `int64_t`) | `DOUBLE` |
| Anything else | `STR` |

The kinds are ordered `UNKNOWN < BOOL < INT < DOUBLE < STR`, and a column
takes the **largest** kind it has seen, so it only ever moves up:
`1, 0, 30, 2.5` ends as `DOUBLE`. One exception: once a column has seen a
bool word, it can't become a number (`yes, 5` is `STR`). Empty values don't
vote. At the end of the sample:

| Column state | Type |
|--------------|------|
| `BOOL` / `INT` / `DOUBLE` | that type |
| `UNKNOWN` (empty in every sampled row) | `DOUBLE`, the most permissive number type |
| `STR` | error: `column 'X' contains text; text columns aren't supported yet` |

The sniffer also checks the number of values per row, so a malformed row in
the sample fails before anything is imported.

**2. Parse strictly (`parse_row`).** Every non-empty value must fully parse
as its column's type: `strtoll` without overflow for `INT`, `strtod` for
`DOUBLE`, one of `true` `false` `t` `f` `yes` `no` `1` `0` for `BOOL`.
Anything else stops the import:

```text
error: line 31222: '2.5' is not a valid INT for column 'qty'
```

So a value outside the sample that doesn't fit its column is an error, never
a silent truncation (`2.5` → `2`) or substitution (`abc` → `0`,
`maybe` → `false`).

**Why `INT` is always `int64_t`.** Choosing `INT8`/`16`/`32` from the sample
would fail on columns whose values grow past the sampled range (IDs,
timestamps, running totals), and every operator would need one code path per
width. Space will instead be saved in storage: in format v2 the writer picks
the narrowest width per row group, when the whole group's min and max are
known, and the reader widens it back to `int64_t`.

**Trade-off.** The sampled rows are read twice, and the input must be
seekable (a regular file, not a pipe).

### Row rules

- An empty field is NULL: bitmap byte 0, value 0.
- Exactly `col_count` fields per row. Fewer fails with "expected N values,
  got M". More, including a trailing comma, fails with "more values than the
  N columns."
- Lines over `BDB_CSV_MAX_LINE` (1 MiB) and fields over `BDB_CSV_MAX_FIELD`
  (255 characters) are rejected.

## Memory ownership

| Memory | Allocated by | Freed by |
|--------|--------------|----------|
| Line buffer | `csv_open` | `csv_close` |
| `chunk.columns` array | `parse_header` (`realloc` per column) | `chunk_free` |
| Each column's `name` | `parse_header` (`strdup`) | `chunk_free` |
| Each column's `data` and `bitmap` | `csv_open`, after sniffing (`calloc`, 2048 slots) | `chunk_free` |
| Sniffer state (one per column) | `csv_sniff` (`calloc`) | `csv_sniff`, before it returns |
| The `CSV_READER` struct itself | the caller (usually on the stack) | the caller |

`chunk_free` sets the pointers to `NULL` and the counts to 0, so calling it
twice is safe. No memory is allocated per row or per field.

## Error handling

Every function that can fail returns a `BdbStatus` and fills in a
`BdbError` with a readable message (`bdb_error_set`). Callers stop on the
first non-`BDB_OK` status and pass it upward. Messages name the line for CSV
errors.

## What's switched off

The first version of BulletDB loaded the whole CSV into one `TABLE` of
`int64_t` columns, then wrote it to `.bdb`, read it back and ran a query.
All of that has since been replaced and deleted (git history keeps it):

| Old part | Replaced by |
|----------|-------------|
| int64 `.bdb` writer and reader | `BDB_WRITER` and `BDB_READER` (Part 2) |
| `query/query.c` (`bdb_run_query` on a `TABLE`) | the executor (Part 3) |

`TABLE` itself remains only as a view for `print_table`: `print_chunk_cb` in
`main.c` fills one from a chunk's `count`, `columns` and `sel_vector`.

---

# Part 2: .bdb v1 file format

## Requirements

1. **Written in one pass, front to back.** The writer receives chunks as a
   stream and doesn't know the total row count until the end. It must never
   seek back.
2. **Fixed writer memory:** one row group buffer, whatever the file's size.
3. **Column-selective reads.** Reading one column of one row group must be
   possible without reading anything else.
4. **Self-describing.** A reader needs nothing but the file: column names,
   types and row counts are all inside it.
5. **Detectably wrong.** A file that isn't `.bdb`, is from a newer version,
   or is cut short must be rejected with a clear error, not misread.

## File layout

```
offset 0
┌─────────────────────────────┐
│ magic   "BDB1"   4 bytes    │  header
│ version u32      4 bytes    │
├─────────────────────────────┤
│ Row group 0                 │
│   col 0: [bitmap][values]   │
│   col 1: [bitmap][values]   │
│   ...                       │
├─────────────────────────────┤
│ Row group 1                 │
│   ...                       │
├─────────────────────────────┤
│ ...                         │
├─────────────────────────────┤
│ FOOTER                      │  metadata: schema + where everything is
├─────────────────────────────┤
│ footer_offset u64  8 bytes  │  trailer
│ magic   "BDB1"   4 bytes    │
└─────────────────────────────┘
end of file
```

All integers are **little-endian**, the native order on x86-64 and ARM64. The
current code writes native byte order, and a big-endian platform would need
byte swapping.

`version` is `1`.

The **footer goes at the end** because it holds things the writer only knows
at the end: the total row count and where each row group landed. This is the
same approach Parquet uses, and it satisfies requirement 1.

## Row groups

A row group holds up to `BDB_ROW_GROUP_SIZE` rows. Every group is full except
possibly the last. Inside a group, each column is **one contiguous block**:

```
column block, for a group of n rows:
┌──────────────────────┬──────────────────────────────┐
│ bitmap: n bytes      │ values: n × type_size bytes  │
│ 1 = valid, 0 = NULL  │ NULL rows hold 0             │
└──────────────────────┴──────────────────────────────┘
```

| Type | Block size for n rows |
|------|-----------------------|
| `INT` | n + 8n = 9n bytes |
| `DOUBLE` | n + 8n = 9n bytes |
| `BOOL` | n + n = 2n bytes |

This is exactly the in-memory layout of a `COLUMN`'s `bitmap` and `data`, so
writing a block is two `fwrite` calls, and reading one is two `fread` calls.

Because each column is one block at a known offset, a query that needs only
`price` seeks to `price`'s block in each group and skips the rest
(requirement 3).

## Footer

```
row_count        u64          total rows in the file
col_count        u16          at most BDB_MAX_COL_COUNT (100)
for each column:
    name_len     u16
    name         name_len bytes, not null-terminated
    type         u8           enum ColumnType value
group_count      u32
for each group:
    row_count    u32          rows in this group (≤ BDB_ROW_GROUP_SIZE)
    for each column:
        offset   u64          byte offset of this column's block
```

Each field is written separately with its exact size, never as a C struct,
so no compiler padding ends up in the file. Values whose in-memory type is
wider (the `ColumnType` enum, `CHUNK.col_count`) are copied into a variable of
the on-disk size before being written.

**Every column in every group has its own offset** (decision 8). Today a
column's offset could be calculated from the group's start, its row count and
the column types, but compressed blocks won't have predictable sizes, so the
format stores each offset rather than relying on that.

Block sizes aren't stored, because they follow from the group's `row_count`
and the column's type (see the table above). Once blocks can be compressed,
a later version will store sizes too.

Footer size: `8 + 2 + Σ(2 + name_len + 1) + 4 + group_count × (4 + 8 × col_count)`
bytes. For 3 columns and 1,000 groups (about 123 million rows), that's about
28 KB.

## Reading a file

```
1. file size < 20 bytes?                     → BDB_ERR_FORMAT (too small)
2. read bytes 0..7: magic, version           → check "BDB1", version == 1
3. read the last 12 bytes: footer_offset, magic
                                             → check "BDB1" (catches truncation)
4. seek to footer_offset, read the footer    → schema + group offsets
5. for each chunk the caller asks for:
     find the group and the row range inside it,
     seek to each needed column's block + row range, fread
```

## Worked example

A real file written by `BDB_WRITER` and checked byte by byte. It comes from
this CSV (one row, five columns):

```csv
ORDERNUMBER,QUANTITYORDERED,PRICEEACH,ORDERLINENUMBER,SALES
10107,30,95.7,2,2871
```

| Offset | Bytes | Contents |
|--------|-------|----------|
| **Header** | | |
| 0x00 | `42 44 42 31` | `"BDB1"` |
| 0x04 | `01 00 00 00` | version = 1 |
| **Row group 0** (1 row) | | |
| 0x08 | `01` + `7B 27 00 00 00 00 00 00` | `ORDERNUMBER`: valid, 10107 |
| 0x11 | `01` + `1E 00 00 00 00 00 00 00` | `QUANTITYORDERED`: valid, 30 |
| 0x1A | `01` + `CD CC CC CC CC EC 57 40` | `PRICEEACH`: valid, 95.7 (IEEE-754 double) |
| 0x23 | `01` + `02 00 00 00 00 00 00 00` | `ORDERLINENUMBER`: valid, 2 |
| 0x2C | `01` + `37 0B 00 00 00 00 00 00` | `SALES`: valid, 2871 |
| **Footer** (starts at 0x35 = 53) | | |
| 0x35 | `01 00 00 00 00 00 00 00` | row_count = 1 |
| 0x3D | `05 00` | col_count = 5 |
| 0x3F | `0B 00` `"ORDERNUMBER"` `00` | 11 characters, INT |
| 0x4D | `0F 00` `"QUANTITYORDERED"` `00` | 15 characters, INT |
| 0x5F | `09 00` `"PRICEEACH"` `01` | 9 characters, DOUBLE |
| 0x6B | `0F 00` `"ORDERLINENUMBER"` `00` | 15 characters, INT |
| 0x7D | `05 00` `"SALES"` `00` | 5 characters, INT |
| 0x85 | `01 00 00 00` | group_count = 1 |
| 0x89 | `01 00 00 00` | group 0: row_count = 1 |
| 0x8D | 5 × u64 | group 0 offsets: 8, 17, 26, 35, 44 |
| **Trailer** | | |
| 0xB5 | `35 00 00 00 00 00 00 00` | footer_offset = 53 |
| 0xBD | `42 44 42 31` | `"BDB1"` |

The file is **193 bytes**. Every pointer leads to the right place: the
trailer points at the footer, and each offset points at a column's bitmap
byte (`01`). This is the first test file for the reader.

A larger check: 160,000 rows of the same five columns give two groups
(122,880 and 37,120 rows). Each column block is `rows × 9` bytes, the data
ends at 8 + 160,000 × 45 = 7,200,008, and every offset starts exactly where
the previous block ended (checked with `bdb_writer_debug_dump`).

## Validation

The reader rejects a file with `BDB_ERR_FORMAT` when any of these are true:

- The file is smaller than 20 bytes (header plus trailer).
- Either magic isn't `"BDB1"`.
- `version` isn't 1.
- `footer_offset` points outside the file, or before byte 8.
- `col_count` is 0 or greater than `BDB_MAX_COL_COUNT`.
- A column type isn't a known, supported `ColumnType`.
- A group's `row_count` is 0 or greater than `BDB_ROW_GROUP_SIZE`.
- The group `row_count`s don't add up to the total `row_count`.
- A block, from its offset plus its computed size, would extend past the
  footer.
- The footer doesn't end exactly where the trailer starts (`file_size − 12`).
  This catches any field read with the wrong size, because every later read
  would be shifted.

A read that ends early fails with `BDB_ERR_IO`.

**What's implemented today:**

| Check | Done in |
|-------|---------|
| File at least 20 bytes | `bdb_reader_open` |
| Header magic and version | `bdb_reader_open` |
| Trailer magic | `bdb_reader_open` |
| Column types are known | `bdb_reader_open` |
| `col_count` within 1..100 | `bdb_reader_debug_dump` only |
| Group row counts within 1..`BDB_ROW_GROUP_SIZE`, and adding up to `row_count` | `bdb_reader_debug_dump` only |
| Blocks contiguous (each starts where the previous ended) | `bdb_reader_debug_dump` only |
| `footer_offset` inside the file | not yet |
| Footer ends at `file_size − 12` | not yet |

The checks that are only in the dump still need to move into
`bdb_reader_open`, so a bad file is rejected even when no dump is called.

## Writer

The writer is a **consumer of chunks**. It collects them into a row group
buffer and writes a group each time the buffer fills up. For a step-by-step
implementation guide with flow diagrams, see
[WRITER_FLOW.md](WRITER_FLOW.md).

```c
typedef struct {
    FILE     *file;
    CHUNK     group;           // row group buffer: capacity BDB_ROW_GROUP_SIZE
    bool      have_schema;     // set by the first append

    // Kept for the footer:
    uint64_t  row_count;       // rows written so far
    uint16_t  col_count;       // on-disk column count
    uint32_t  group_count;     // groups written so far
    uint32_t *group_rows;      // [group_count]            rows in each group
    uint64_t *offsets;         // [group_count * col_count] offset of each column block
    uint32_t  groups_capacity; // allocated length of group_rows (offsets: × col_count)
} BDB_WRITER;

BdbStatus bdb_writer_open  (BDB_WRITER *w, const char *path, BdbError *err);
BdbStatus bdb_writer_append(BDB_WRITER *w, const CHUNK *chunk, BdbError *err);
BdbStatus bdb_writer_finish(BDB_WRITER *w, BdbError *err);
void      bdb_writer_close (BDB_WRITER *w);
```

| Function | Does |
|----------|------|
| `open` | Zeroes the struct, opens the file and writes the header (`"BDB1"`, version). |
| `append` | On the first chunk, copies the schema (names, types) into the group buffer and allocates it (`init_group`). Then `memcpy`s the chunk's values and bitmap onto the end of the buffer. When the buffer holds exactly `BDB_ROW_GROUP_SIZE` rows, writes the group. |
| `write_group` (static) | Grows `group_rows` and `offsets` if they're full. For each column, records its offset with `bdb_ftell` (64-bit, so offsets past 2 GB work), then writes its bitmap and values. Stores the group's row count, then increments `group_count`, and resets the buffer. |
| `finish` | Writes the last, partial group if there is one. Records where the footer starts, writes the footer, then the trailer (that position plus `"BDB1"`), and flushes. Every write is checked. |
| `close` | Frees the buffer and both arrays and closes the file. Safe to call after an error, and safe to call twice. It doesn't write anything, so a file closed without `finish` has no footer. |

**Growing the arrays.** The number of groups isn't known in advance, so
`group_rows` and `offsets` grow by doubling (`GROW_CAPACITY` / `GROW_ARRAY` in
`storage/memory.h`, the same approach as clox). Capacity starts at 0 with
`NULL` arrays. `write_group` grows only when `group_count == groups_capacity`,
and the first grow (0 → 8) allocates through `realloc(NULL, ...)`. `offsets`
is sized `capacity × col_count`.

**Debugging.** `bdb_writer_debug_dump` in `storage/debug.c` prints the
schema, each group's row count and each column's offset, and marks any block
that doesn't start where the previous one ended. Call it after `finish` and
before `close`.

Because a row group is always a whole number of chunks, a chunk is always
copied into one group in a single step, never split across two.

`main` becomes:

```c
csv_open(&reader, csv_path, &err);
bdb_writer_open(&writer, "test.bdb", &err);
while (/* csv_next_chunk gives a chunk */) {
    bdb_writer_append(&writer, chunk, &err);
}
bdb_writer_finish(&writer, &err);
bdb_writer_close(&writer);
csv_close(&reader);
```

**Memory:** one group buffer of `BDB_ROW_GROUP_SIZE × (1 + type_size)` bytes
per column, about 1.1 MB per `INT` or `DOUBLE` column, plus `4 + 8 ×
col_count` bytes of metadata per group (its row count and one offset per
column). The size of the CSV doesn't matter.

**Known gap:** the schema comes from the first chunk. A CSV with a header but
no data rows never calls `append`, so the file gets a footer with 0 columns
and the header's names are lost.

## Reader

The reader is a **source**, with the same shape as `CSV_READER`.

| Milestone | What | Status |
|-----------|------|--------|
| M1 | `bdb_reader_open`: header, trailer and footer loaded and checked | **Done**, verified with `bdb_reader_debug_dump` |
| M2 | `bdb_reader_next_chunk` + `bdb_reader_close`: stream the data one chunk at a time | **Done**: 225,721 rows come back as 111 chunks |
| M3 | Round trip with values (CSV → `.bdb` → reader gives the same values), then `SUM` | Next |

```c
typedef struct {
    FILE      *file;

    // From the footer (the only metadata held in memory):
    uint64_t   row_count;
    uint16_t   col_count;
    uint32_t   group_count;
    uint32_t  *row_groups;    // [group_count]            rows in each group
    uint64_t  *offsets;       // [group_count * col_count] offset of each column block

    // Schema (names, types in chunk.columns) plus buffers for
    // BDB_VECTOR_SIZE rows per column, allocated once at the end of open and
    // reused by every next_chunk:
    CHUNK      chunk;

    // Position for next_chunk:
    uint32_t   group_index;   // current row group
    uint64_t   rows_in_group; // next row to read, counted from the start of that group
} BDB_READER;

BdbStatus bdb_reader_open      (BDB_READER *r, const char *path, BdbError *err);
BdbStatus bdb_reader_next_chunk(BDB_READER *r, const CHUNK **out, BdbError *err);
void      bdb_reader_close     (BDB_READER *r);
```

Using it looks exactly like the CSV reader:

```c
BDB_READER reader = {0};
const CHUNK *chunk;

BdbStatus status = bdb_reader_open(&reader, "test-1.bdb", &err);
while (status == BDB_OK &&
       (status = bdb_reader_next_chunk(&reader, &chunk, &err)) == BDB_OK && chunk != NULL) {
    /* use chunk */
}
bdb_reader_close(&reader);
```

**How `open` reads the footer.** It seeks to `footer_offset`, reads
`row_count` and `col_count`, allocates `chunk.columns`, then reads each
column's name (into a `malloc`ed buffer with a `'\0'` added, since the file
doesn't store one) and type (read into a `uint8_t`, checked, then assigned to
the enum). Then it reads `group_count` and, for each group, its row count
followed by its `col_count` offsets in one `fread`.

Every field is read with its exact on-disk size. Reading one field with the
wrong size shifts every read after it. That happened twice while building
this: `group_count` read as 2 bytes instead of 4 made the first group's row
count come out as 65,536, and reading the row count twice made it 8. Using
`sizeof(the_field)` instead of `sizeof(some_type)` prevents this.

**Debugging.** `bdb_reader_debug_dump` in `storage/debug.c` prints what
`open` loaded, in the same layout as the writer's dump, and checks it
(column count, types, group row counts and their total, contiguous blocks).
Problems are printed with `!!`, and it returns how many it found. Checked so
far: the 193-byte example gives 0 problems, and a 225,721-row file (2 groups)
gives a dump identical to the writer's.

**Only the header and footer are held in memory.** Row groups are never
loaded whole: data is streamed one chunk at a time.

```
bdb_reader_open
  ├─ header  (8 bytes)    read → check "BDB1" + version → discarded
  ├─ trailer (12 bytes)   read → footer_offset, check magic → discarded
  └─ footer               read → kept: row_count, schema, group_rows, offsets

bdb_reader_next_chunk     one CHUNK buffer (2048 rows), reused
```

- `open` validates the file, loads the footer, and allocates the chunk's
  buffers (2048 values and 2048 bitmap bytes per column).
- `next_chunk` reads the next rows of the current group:

  ```
  *out = NULL
  group_index == group_count ?                → return OK (no rows left)

  n     = row_groups[group_index]              rows in this group
  count = min(2048, n − rows_in_group)

  for each column c:
      base = offsets[group_index × col_count + c]
      bitmap: seek base + rows_in_group                read count bytes
      values: seek base + n + rows_in_group × size     read count × size bytes

  chunk.count = count
  rows_in_group += count
  rows_in_group == n ?  → group_index++, rows_in_group = 0
  *out = &chunk
  ```

  The values start after the **whole** bitmap, so their seek adds `n` (the
  group's row count), not `rows_in_group`. A short read or a failed seek
  returns `BDB_ERR_IO`. After the last group it keeps returning `NULL`.
- `close` frees the column names, the chunk buffers and the footer arrays,
  and closes the file. It's safe after an `open` that failed partway,
  because the struct starts zeroed and `chunk.columns` comes from `calloc`.

Because a row group is a whole number of chunks (except the last group), a
chunk never spans two groups. A chunk has fewer than 2048 rows only at the end
of a group, which in practice means the last chunk of the file. For 225,721
rows in groups of 122,880 and 102,841: 60 chunks, then 50 chunks plus one
of 441 rows, 111 in total.

Memory therefore doesn't depend on the file size: the footer is 8 bytes per
column per group plus 4 per group, and the chunk buffer is about 18 KB per
`INT` column.

Seeks must be 64-bit because plain `fseek` takes a `long`, which is 32-bit on
Windows. All seeks and tells go through `bdb_fseek` / `bdb_ftell` in
`storage/io.h`, which map to `_fseeki64` / `_ftelli64` on Windows and
`fseeko` / `ftello` on POSIX.

**The round-trip test:** CSV → writer → `test-1.bdb` → reader, then compare
with the CSV's chunks. Same row count (done: 225,721 rows in 111 chunks),
same values and same bitmaps (next: compare `SUM` of each column from both
sides; the values go through the file unchanged and in the same order, so the
sums must be exactly equal, not just close).

**Mistakes made while building it**, worth remembering for the next reader or
writer:
- Passing `&col->bitmap` (the address of the pointer) to `fread` instead of
  `col->bitmap` (the buffer). It writes over the `COLUMN` struct itself.
- Seeking to `base + rows_in_group + rows_in_group × size` for the values,
  which skips only part of the bitmap. The bitmap is `n` bytes long.
- Reading `BDB_VECTOR_SIZE` rows instead of `count`, which runs past the end
  of the group's block for the last chunk.
- A missing `return BDB_OK;` at the end of `next_chunk`: harmless while the
  caller ignored the status, but it broke the loop as soon as the status was
  checked. `-Werror=return-type` makes this a build error.

Later, `next_chunk` can take a list of wanted columns and read only those.
That's the payoff of the columnar layout.

## Decisions

| # | Decision | Chosen | Why | Revisit when |
|---|----------|--------|-----|--------------|
| 1 | Where metadata goes | **Footer** at the end | The writer only knows row counts and offsets at the end, and it must write in one pass | — |
| 2 | Per-group statistics (min, max, null count) | **Not in v1** | Keeps v1 small. The version field allows adding them. | The query engine can use them to skip groups |
| 3 | Bitmap on disk | **1 byte per row** | Matches memory exactly, so a block is one `fwrite` | NULL bitmaps are packed to 1 bit in memory |
| 4 | Byte order | **Little-endian (native)** | Every target machine is little-endian | A big-endian target appears |
| 5 | Column block order | **Bitmap first, then values** | Either works. This fixes one. | — |
| 6 | Block sizes in the footer | **Not stored, computed** | Follows from row count and type | Compression makes sizes variable |
| 7 | Group row count | **Stored for every group** | Simple and explicit, with no special case for the last group | — |
| 8 | Where blocks are found | **An offset per column per group** (not one offset per group) | A reader can seek straight to any column, and the format keeps working once compressed blocks have unpredictable sizes. Costs 8 bytes per column per group. | — |
| 9 | Field sizes | `col_count` and `name_len` are **u16**, `group_count` and group `row_count` are **u32**, `row_count` and offsets are **u64** | Big enough for every limit, with no wasted bytes | A limit changes |
| 10 | Reader memory | **Header and footer only**; data streamed in chunks | Memory stays fixed whatever the file size | — |

---

# Part 3: Query execution

## The model: Volcano with vectors

A query runs as a **tree of operators**. Each operator has a `next` function
that the operator above it calls to get data; each operator gets its own
input by calling `next` on its **child**. This is the Volcano (iterator)
model (Graefe, 1994).

Classic Volcano returns **one row** per `next`. BulletDB returns **one chunk
of up to 2048 rows**, which is vectorized execution (MonetDB/X100, Boncz et
al., 2005; also DuckDB). The difference matters for speed: with one row per
call, every row pays for a function call through a pointer in every
operator, plus a type check, a few nanoseconds each. A `SUM` loop over a
chunk costs about 0.45 ns per row. With chunks, those costs are paid once per
2048 rows, and the work inside is a plain loop over an array.

The analogy with Crafting Interpreters: the operator tree is like an AST, and
running it is like a tree-walking interpreter. Tuple-at-a-time is jlox
(paying the tree-walking cost for every row); vectorized execution keeps the
tree but does a whole batch per node visit; compiled engines (HyPer) are the
clox approach, turning the query into code.

The model is **pull-based**, the same as the CSV and `.bdb` readers: the
caller asks for the next chunk. (DuckDB later moved to push-based execution,
mainly to parallelize better. That only matters once there are threads.)

## The base operator

`executor/bdb_operator.h`:

```c
typedef struct BdbOperator BdbOperator;

struct BdbOperator {
    BdbStatus (*next)(BdbOperator *self, const CHUNK **out, BdbError *err);
    void      (*close)(BdbOperator *self);
    void      (*describe)(BdbOperator *self, FILE *out);
    BdbOperator *child;
    uint64_t    stat_calls;     // times bdb_op_next was called on this operator
    uint64_t    stat_chunks;    // non-NULL chunks returned
    uint64_t    stat_rows;      // total rows emitted
    uint64_t    stat_time_ns;   // total wall time inside this operator's next(), in nanoseconds
};
```

- **`next`** sets `*out` to the next chunk, or to `NULL` when there is no more
  data. The chunk belongs to the operator and is overwritten by the next call.
- **`close`** frees the operator **and closes its child**, so closing the top
  operator closes the whole tree.
- **`describe`** prints one line about the operator, for `bdb_explain`. After
  execution it also prints the stat counters (see D2/D3 below).
- **`child`** is the operator it pulls from, or `NULL` for a scan.

Callers never call `op->next` directly. They call `bdb_op_next(op, &out, err)`,
a static inline wrapper in `bdb_operator.h` that increments `stat_calls`,
times the call with `bdb_now()`, and updates `stat_chunks`, `stat_rows` and
`stat_time_ns` from the result.

Each concrete operator is a struct that **embeds `BdbOperator` as its first
field** and adds its own state. Because a struct's first field starts at the
struct's own address, a pointer to the operator and a pointer to its `base`
are the same address. This is how C does inheritance, the same trick as `Obj`
at the start of every object in clox:

```c
typedef struct {
    BdbOperator base;     // must be first
    BDB_READER  reader;   // this operator's own state
} BdbScan;
```

- **Upcast** (operator → base), done by callers: `&scan.base`. No cast needed.
- **Downcast** (base → operator), done inside the operator's own functions:
  `BdbScan *scan = (BdbScan *)self;`.

Each operator's constructor sets the function pointers to its own static
functions (`scan->base.next = scan_next;`). After that, anyone calling
`op->next(op, ...)` ends up in the right function without knowing what kind
of operator it is (dynamic dispatch, like a virtual method). The functions
themselves are `static`: they're only reached through the pointers.

**Include direction:** operator headers include `bdb_operator.h`; the base
header never includes any operator. An early version had the two headers
include each other, which broke depending on include order.

## Scan

`executor/scan.c`. The bottom of every tree. It **owns** a `BDB_READER`:

| Function | Does |
|----------|------|
| `bdb_scan_open(scan, path, err)` | Zeroes the struct, sets the function pointers and `child = NULL`, then opens the reader. The pointers are set first, so `close` is safe even if opening fails. |
| `scan_next` | Returns `bdb_reader_next_chunk` on its reader. |
| `scan_close` | Closes the reader. |
| `scan_describe` | `SCAN (225721 rows, 5 columns, 2 groups)` before a run; after a run appends `calls: N  chunks: N  rows: N  time: N ms  (times next() was invoked \| non-empty chunks returned \| total rows emitted)` |

## Aggregate

`executor/aggregate.c`. Computes one aggregate of one column over everything
its child produces.

| Aggregate | Keeps | Result | Result type | No non-NULL values |
|-----------|-------|--------|-------------|--------------------|
| `SUM` | running total | total | the column's type | NULL |
| `COUNT` | number of non-NULL values | count | INT | 0 |
| `MIN` | smallest value so far | min | the column's type | NULL |
| `MAX` | largest value so far | max | the column's type | NULL |
| `AVG` | total and count | total ÷ count | DOUBLE | NULL |

```c
typedef enum { BDB_AGG_SUM, BDB_AGG_COUNT, BDB_AGG_MIN, BDB_AGG_MAX, BDB_AGG_AVG } BdbAggregateType;

typedef struct {
    BdbOperator      base;
    const char      *column_name;   // e.g. "PRICEEACH"
    uint64_t         column;        // its index, found in the first chunk
    bool             found_column;
    BdbAggregateType type;          // which aggregate (SUM, COUNT, ...)

    enum ColumnType  col_type;      // the column's type
    int64_t          int_sum;       // INT columns: exact integer total
    double           double_sum;    // DOUBLE columns
    uint64_t         count;         // non-NULL values seen
    bool             has_value;     // has the first value been seen? (MIN/MAX)
    int64_t          int_min,    int_max;
    double           double_min, double_max;

    bool             done;          // result already returned?
    CHUNK            result;        // 1 column × 1 row
} BdbAggregate;

void bdb_aggregate_init(BdbAggregate *agg, BdbOperator *child,
                        BdbAggregateType type, const char *column_name);
```

`next` does this:

```
*out = NULL
done ?                                    → return OK (the result was already given)

loop: chunk = child->next()
    error?                                → return it
    NULL?                                 → stop (the child is finished)
    first chunk: find the column by name  → BDB_ERR_NOT_FOUND if it's not there
    switch on the COLUMN type (once per chunk):
        cast data to int64_t* or double*
        for every row whose bitmap byte is 1:
            count++, sum += v
            first value ever?  → min = max = v
            otherwise          → update min and max

build the result (switch on the AGGREGATE type)
done = true
*out = &result
```

**The loop doesn't depend on which aggregate was asked for.** It updates
count, sum, min and max together, for every kind. Only `build_result` looks
at the aggregate type, to decide what to report. That keeps two separate
questions in two separate places:

| Where | Depends on | Decides |
|-------|------------|---------|
| the loop | the **column** type | how to read the values (`int64_t *` or `double *`) |
| `build_result` | the **aggregate** type | which total to report, and its result type |

It costs a few extra comparisons per row compared with one specialized loop
per aggregate, and means adding an aggregate needs no change to the loop.
(An early version switched on the aggregate type inside the loop and on the
column type inside that, which grew a new nested branch for every aggregate.)

`build_result`:

```
result type:  COUNT → INT,  AVG → DOUBLE,  otherwise the column's type
allocate:     1 COLUMN, named "<AGG>(<column>)" from the AGG_NAMES table
no values and not COUNT?          → bitmap[0] = 0 (NULL), done
switch on the aggregate type      → write the one value, cast to int64_t* or double*
bitmap[0] = 1
```

C can't choose a pointer type at run time, so each case writes through a
cast it knows in advance (`is_int ? int_min : double_min`, and so on). AVG
converts the total to `double` before dividing, so the average of an INT
column isn't cut to a whole number.

Details worth noting:
- **The column is found by name in the first chunk it receives,** not in a
  schema. The aggregate only knows it has *some* child operator, and
  `BdbOperator` has no schema, but every chunk carries its column names. So
  this works whatever sits below it.
- **`data` is a `void *`**, because a column's type is only known at run
  time. The loop casts it to `int64_t *` or `double *` once per chunk, based
  on the column's type, and the `switch` is also once per chunk, never once
  per row.
- **INT columns are summed into an `int64_t`**, so the result is exact. A
  `double` would lose precision above about 9 × 10¹⁵.
- **The first non-NULL value becomes both min and max,** without comparing
  (tracked by `has_value`). Starting `min` at 0 would be wrong: if every value
  is above 0, the minimum would come out as 0, a value that isn't in the data.
- **NULLs are skipped by every aggregate.** With no non-NULL values at all,
  `COUNT` is 0 and the others are NULL (`bitmap[0] = 0`), as in SQL.
- **Keep the two types apart.** An early version compared the aggregate type
  with column-type constants. It passed by accident because `BDB_AGG_SUM` and
  `BDB_AGG_COUNT` happen to have the same numbers (0 and 1) as `BDB_COL_INT`
  and `BDB_COL_DOUBLE`, and failed as soon as `MIN` (2) was added. The field
  holding the aggregate type is still named `type`; `kind` would make the
  difference from `col_type` clearer.
- **The result is an ordinary `CHUNK`,** so it prints with `print_table` and
  could be consumed by another operator. Its memory is allocated with
  `calloc` and freed by `chunk_free` in `close`.
- **`done` is a separate idea from "the child is finished".** The child being
  finished (its `next` returns `NULL`) ends the pulling loop. `done` remembers
  that the result has been returned, so the *second* call to the aggregate's
  `next` returns `NULL` instead of pulling again and producing a second result.

Verified: `SUM` of every column of the 225,721-row test file matches the sums
computed directly from the CSV, and `COUNT`, `MIN`, `MAX`, `AVG` and the NULL
cases were checked by hand.

## Filter

`executor/filter.c`. `WHERE column op value`: a streaming operator that keeps
the rows of each chunk that match one comparison against a number.

```c
typedef enum { BDB_COMPARE_EQ, BDB_COMPARE_NE, BDB_COMPARE_LT,
               BDB_COMPARE_LE, BDB_COMPARE_GT, BDB_COMPARE_GE } BdbCompareOp;

typedef struct {
    BdbOperator      base;
    const char      *column_name;
    BdbCompareOp     op;
    uint64_t         column;          // its index, found in the first chunk
    bool             found_column;
    enum ColumnType  col_type;
    const char      *value;           // the constant as given, for describe
    double           number;          // the same constant, parsed once
    uint16_t         sel[BDB_VECTOR_SIZE];   // the selection it returns
    CHUNK            out_chunk;       // borrows the child's columns
} BdbFilter;

bdb_filter_init(&filter, &scan.base, "QUANTITYORDERED", BDB_COMPARE_GE, "45");
```

How `next` works:

1. Pull one chunk from the child. On the first chunk, find the column by name
   (`BDB_ERR_NOT_FOUND` if it isn't there). Only `INT` and `DOUBLE` columns
   can be filtered (`BDB_ERR_INVALID` otherwise).
2. Loop over the chunk's rows, following its `sel_vector` if it has one, and
   write the physical row number of each match into `filter->sel`.
3. Return `out_chunk`: the **child's own columns**, `count` = the number of
   matches, `sel_vector = filter->sel`. Nothing is copied.

Details:

- **NULL never matches,** not even `!=`, as in SQL. So `qty = 40` and
  `qty != 40` together cover every non-NULL row, not every row.
- **No matches gives an empty chunk.** When nothing in a chunk matches, the
  filter returns `count = 0` instead of pulling again. Consumers skip it:
  the aggregate adds nothing, and `main.c` doesn't print it.
- **Filters stack.** A second filter receives an already-selected chunk,
  follows its selection, and stores physical rows, so two filters make an
  `AND`.
- **Branchless loop.** The loop writes every candidate row into
  `sel[matches]` and then adds `bitmap[k] != 0 & (v[k] op x)` to `matches`,
  so there's no branch for the CPU to mispredict. A non-match is overwritten
  by the next row. `FILTER_LOOP` and `FILTER_OPS` generate one loop per type
  and comparison, so the comparison is chosen once per chunk, not per row.
- **The constant is parsed once,** with `strtod` in `bdb_filter_init`. Two
  known gaps come from that (see [KNOWN_ISSUES.md](KNOWN_ISSUES.md)): a
  fractional constant on an `INT` column, and integer constants above 2⁵³.
- `describe` prints `FILTER QUANTITYORDERED >= 45`, followed by the same
  statistics as the other operators after a run.

## Streaming operators and pipeline breakers

Operators fall into two groups:

| Kind | Examples | Behaviour of `next` |
|------|----------|---------------------|
| **Streaming** | scan, filter, projection | handles each chunk as it arrives and passes it up straight away |
| **Pipeline breaker** | aggregate, sort, hash-join build | must see **all** its input first: pulls from its child until `NULL`, then returns results |

A chain of streaming operators ending at a breaker is a **pipeline**. Today's
queries are a single pipeline (scan → aggregate). A sort or join would split
a query into several. The order of operators is fixed by the tree's shape,
which is built before running; at run time each operator only knows its
child.

## Explain and the execution debugger

`executor/explain.c`: `bdb_explain(op, out)` walks down the `child` pointers
and prints each operator's `describe` line, indented by depth, like SQL's
`EXPLAIN`:

```
AGGREGATE SUM(PRICEEACH)
        +-SCAN (225721 rows, 5 columns, 2 groups)
```

It only uses `describe` and `child`, so it works for any operator, including
ones not written yet.

The rest of the debugger is planned in steps:

| Step | What | How |
|------|------|-----|
| D1 | Plan printer | `describe` + `bdb_explain` (**done**) |
| D2 | Counts per operator: `next` calls, chunks and rows returned | `bdb_op_next(op, &out, err)` wrapper increments `stat_calls`, `stat_chunks`, `stat_rows` in the operator's base on every call. Every `next` call goes through it. `bdb_explain` prints the counts after a run. (**done**) |
| D3 | Time per operator | `bdb_op_next` also brackets the call with `bdb_now()` and accumulates `stat_time_ns`. Each `describe` prints total wall time in ms. For pipeline breakers (aggregate), `describe` subtracts the child's `stat_time_ns` to show self time only. (**done**) |
| D4 | Tracing | The wrapper prints the first rows of each chunk leaving an operator that's marked for tracing |

The idea behind the wrapper is the same as tracing in clox's `run()` loop:
one hook in the place everything passes through, instead of `printf` calls
scattered through every operator.

## Next steps

**Step D4: tracing** (next in the debugger). The `bdb_op_next` wrapper prints
the first few rows of each chunk for operators marked for tracing, without
touching any operator's own code.

**Zone maps (v0.2).** Per-group `min`/`max` in the footer (format v2), so the
scan can skip whole row groups that a filter can't match. See
[Optimizer](#optimizer).

## Optimizer

The optimizer sits between plan construction and execution. It transforms the
operator tree to avoid work — skipping data the query can't use — without
changing the result.

### Zone-map optimizer (planned for v0.2)

The first and most impactful pass uses **per-group min/max statistics** (zone
maps) stored in the footer (see v2 format, below). Before scanning a row
group, the optimizer classifies it against the query's filter predicate:

| Outcome | Condition | Action |
|---------|-----------|--------|
| `SKIP`  | `group_max < predicate_value` (or equivalent) | No I/O; group contributes nothing |
| `FULL`  | `group_min >= predicate_value` — every row matches | Use stored `sum`/`null_count` directly; skip the scan |
| `SCAN`  | Otherwise — uncertain | Stream chunks through the normal vector path |

```
                    sales.bdb
                       │
               ┌───────┴────────┐
               │ row-group stats │
               └───────┬────────┘
                        │
           ┌────────────┼────────────┐
           ↓            ↓            ↓
        group 1      group 2      group 3
        all match    none match   uncertain
           │            │            │
           ↓            ↓            ↓
      metadata SUM    skip       vector scan
```

For a `FULL` group, `BdbAggregate` needs a second accumulation path —
`accumulate_from_stats(sum, count, min, max)` — so the result is correct
whether a group was scanned or not.

This requires the v2 footer additions described in "Later" below.

### Per-operator optimization (later passes)

Once zone maps exist, each operator gets its own deeper optimization layer:

| Operator | Level 1 (zone maps) | Level 2 (row-level) | Level 3 (expression) |
|----------|---------------------|---------------------|----------------------|
| **Filter** | Skip non-matching groups | SIMD predicate evaluation on uncertain groups | Late materialization — don't load non-predicate columns until after filtering |
| **Aggregate** | Accumulate `FULL` groups from stats, skip `SKIP` groups | Tight accumulation loop, SIMD-friendly | Pre-aggregation per group before combining |
| **Join** | Bloom filter on build side to pre-filter probe groups | Hash join vs. merge join based on size estimates | Partition pruning |

The pattern is the same every time: zone maps give **group-level** decisions
for free once the stats exist; each operator then adds its own **row-level**
and **expression-level** tricks on top.

## Execution decisions

| # | Decision | Chosen | Why |
|---|----------|--------|-----|
| E1 | Processing model | **Volcano iterator, one chunk per `next`** (vectorized) | Per-row function calls would cost more than the actual work; chunks already come out of storage |
| E2 | Pull or push | **Pull** | Matches the CSV and `.bdb` readers. Push helps parallel execution, which doesn't exist yet |
| E3 | Operator "classes" | **Base struct as first field + function pointers** | Callers work with any operator; adding one needs no central `switch` |
| E4 | Who closes the child | **Each operator closes its child** | `main` closes only the top operator |
| E5 | Finding columns | **By name, in the first chunk received** | Works with any child; no schema needed on `BdbOperator` |
| E6 | Filter output | **Selection vectors** over the child's columns; an empty chunk (`count = 0`) when nothing matches | No copying; the planned copy-first version was skipped |
| E7 | Aggregate kinds | **One operator; one loop updates count, sum, min and max; the result picks one** | Adding an aggregate doesn't touch the loop; costs a few comparisons per row |
| E8 | Aggregate result types | **COUNT → INT, AVG → DOUBLE, SUM/MIN/MAX → the column's type**; NULL when there are no values (COUNT: 0) | Matches SQL |

---

## Later

Roughly in order, once v1 works end to end:

1. **Finish the executor basics** (Part 3, Next steps): tracing (D4) in the
   execution debugger. (`FILTER` is done.)
2. **Column projection.** The reader loads only the columns a query uses.
3. **v2: row-group statistics.** Add per-column per-group stats to the footer,
   stored alongside each group's existing `offset`:

   ```
   for each group:
       row_count    u32
       for each column:
           offset      u64          (already in v1)
           min         8 bytes      typed the same as the column
           max         8 bytes
           null_count  u32
           sum         8 bytes      (0 for BOOL)
   ```

   The writer already sees every value during `bdb_writer_append`, so
   tracking `min`/`max`/`sum`/`null_count` is a per-row comparison added to
   the copy loop — essentially free. When `bdb_write_group` fires, the
   accumulators are committed to the footer arrays and reset.

   These stats are the prerequisite for the zone-map optimizer (see above):
   a `SKIP` group costs zero I/O; a `FULL` group costs only a footer read.
4. **Packed bitmaps:** 1 bit per row, stored as `uint64_t` words (32 words per
   2048-row chunk), in memory and on disk.
5. **Strings,** stored with a dictionary.
6. **Compression** per column block: run-length, delta, dictionary. Block
   sizes then go into the footer.
7. **Vectorized execution:** filter and aggregate loops over whole chunks,
   written so the compiler can use SIMD.
8. **Aligned blocks.** Blocks are currently packed back to back, so values
   often start at odd offsets (for example 7,380,746 + 102,841 for a `DOUBLE`
   block). That's fine with `fread`, which copies into aligned buffers, but
   memory-mapping the file and reading values in place needs each block padded
   to a multiple of 8 (or 64) bytes, as Parquet and Arrow do. This is a
   format change, so it belongs in v2.
9. **Narrower integer types** (`INT8`, `INT16`, `INT32`) when the schema is
   known in advance (for example from a SQL source), and automatic
   per-group bit widths (frame-of-reference and bit-packing) as part of
   compression. Today every integer takes 8 bytes, which is why a `.bdb` file
   is about 1.9 times the size of the CSV it came from.
