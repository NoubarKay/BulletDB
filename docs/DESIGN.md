# BulletDB design

This document describes how BulletDB is built today, and the `.bdb` v1 file
format with its writer and reader.

- Part 1, [Current design](#part-1-current-design), covers the data model and
  the CSV side.
- Part 2, [.bdb v1](#part-2-bdb-v1-file-format), covers the file format. The
  **writer is built** and produces files that match the spec byte for byte.
  The **reader is half built**: `bdb_reader_open` loads and checks the header,
  trailer and footer (milestone 1). Streaming the data (`next_chunk`) is next.

The README covers building, usage and the list of known bugs. This document
covers the structure of the engine and the reasons behind it.

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
| CSV | `csv/csv_tokenize.c`, `csv/csv_reader.c` | Read a line, split fields (`next_field`); header, type detection, lines → chunks. `csv_tokenize.h` holds the CSV limits |
| Storage | `storage/bdb_writer.c`, `storage/bdb_reader.c`, `storage/bdb_format.h` | Chunks → `.bdb` file, and `.bdb` file → footer (data streaming next). `bdb_format.h` holds the magic, version and file limits |
| Storage helpers | `storage/memory.c`, `storage/io.c`, `storage/debug.c` | `GROW_CAPACITY` / `GROW_ARRAY`, a checked `fread` helper, `bdb_writer_debug_dump` / `bdb_reader_debug_dump` |
| Query | `query/query.c` | Old int64 query engine, switched off |

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
    bool     have_types;   // set once the first data row has fixed the types
    CHUNK    chunk;        // reused for every chunk
} CSV_READER;
```

### Lifecycle

```
csv_open
  ├─ fopen, allocate the line buffer
  └─ read line 1 → parse_header → one COLUMN per name (data and bitmap NULL)

csv_next_chunk  (called repeatedly)
  ├─ chunk_reset
  └─ loop until the chunk is full or the file ends:
       read_next_line          (EOF → stop)
       blank line?             → skip
       first data row?         → detect types on a copy of the line,
                                 allocate data and bitmap for each column
       parse_row               → store values at [chunk.count]
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
than the earlier version, which copied every field (see the README's
Performance section). Consequences:

- A line can only be split once, so type detection on the first row works on
  a `strdup` copy.
- A field pointer is only valid until the next line is read. Numbers and BOOL
  values are converted immediately. Strings, once they exist, must be copied.

### Type detection

Types are fixed by the **first data row**:

| Value | Type |
|-------|------|
| `true` `false` `t` `f` `yes` `no` (any case) | `BOOL` |
| Parses fully as an integer | `INT` |
| Parses fully as a decimal | `DOUBLE` |
| Anything else, including empty | `STR`, which fails the import |

Later rows aren't checked against these types. Improving this is on the
roadmap.

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
| Each column's `data` and `bitmap` | first data row (`calloc`, 2048 slots) | `chunk_free` |
| Copy of the first row | `csv_next_chunk` (`strdup`) | `csv_next_chunk`, right away |
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
`int64_t` columns, then wrote it to `.bdb`, read it back and ran a query. The
old writer and reader have been deleted (the new writer in Part 2 replaces
them, and git history keeps them). One part is still in the tree but not
called:

| Part | File | State |
|------|------|-------|
| Query engine | `query/query.c` | int64 only, takes a `TABLE`, scan loop commented out |

It gets ported to chunks once the new reader exists.

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
| `write_group` (static) | Grows `group_rows` and `offsets` if they're full. For each column, records its offset with `_ftelli64` (64-bit, so offsets past 2 GB work), then writes its bitmap and values. Stores the group's row count, then increments `group_count`, and resets the buffer. |
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
| M2 | `bdb_reader_next_chunk`: stream the data one chunk at a time | Next |
| M3 | Round trip (CSV → `.bdb` → reader gives the same rows), then `SUM` | After M2 |

```c
typedef struct {
    FILE      *file;

    // From the footer (the only metadata held in memory):
    uint64_t   row_count;
    uint16_t   col_count;
    uint32_t   group_count;
    uint32_t  *row_groups;    // [group_count]            rows in each group
    uint64_t  *offsets;       // [group_count * col_count] offset of each column block

    // Schema (names, types in chunk.columns); later also the buffers for
    // BDB_VECTOR_SIZE rows, reused by next_chunk:
    CHUNK      chunk;

    // Planned for M2:
    // uint32_t group;          current group
    // uint32_t row_in_group;   next row to read in it
} BDB_READER;

BdbStatus bdb_reader_open      (BDB_READER *r, const char *path, BdbError *err);  // done
BdbStatus bdb_reader_next_chunk(BDB_READER *r, const CHUNK **out, BdbError *err); // M2
void      bdb_reader_close     (BDB_READER *r);                                   // to add
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

- `open` validates the file and loads the footer.
- `next_chunk` reads the next (up to) 2048 rows of the current group. For
  each column, it seeks to `offset + row_in_group` for the bitmap and to
  `offset + n + row_in_group × type_size` for the values, where `n` is the
  group's row count. When a group runs out, it moves to the next group. It
  returns `NULL` at the end.

Memory therefore doesn't depend on the file size: the footer is 8 bytes per
column per group plus 4 per group, and the chunk buffer is about 18 KB per
`INT` column.

Seeks must be 64-bit (`_fseeki64` on Windows, `fseeko` elsewhere), because
plain `fseek` takes a `long`, which is 32-bit on Windows.

**The round-trip test:** CSV → writer → `test.bdb` → reader, then compare
each chunk with the CSV's chunks. Same row count, same values, same bitmaps.

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

## Later

Roughly in order, once v1 works end to end:

1. **Query engine on chunks.** It pulls from `BDB_READER`, handles types and
   NULLs, and adds `COUNT`, `MIN`, `MAX` and `AVG`.
2. **Column projection.** The reader loads only the columns a query uses.
3. **v2: statistics.** `null_count`, `min` and `max` per column per group, so
   a filter like `year = 2025` can skip whole groups.
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
