# BulletDB design

This document describes how BulletDB is built today, and the design of the
next piece: the `.bdb` v1 file format with its writer and reader.

- Part 1, [Current design](#part-1-current-design), is what's in the code now.
- Part 2, [.bdb v1](#part-2-bdb-v1-file-format-proposed), is agreed but not
  built yet.

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
- [Part 2: .bdb v1 file format (proposed)](#part-2-bdb-v1-file-format-proposed)
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
┌──────────────────────────────┐
│ CSV_READER  (src/csv.c)      │
│   next_field splits a line   │
│   parse_row fills ──▶ CHUNK  │   up to BDB_VECTOR_SIZE rows, reused
└──────────────┬───────────────┘
               │ csv_next_chunk(&reader, &chunk, &err)
               ▼
      ┌──────────────────┐
      │ main.c           │   prints each chunk
      └──────────────────┘
```

| Layer | Files | Job |
|-------|-------|-----|
| Tokenizer | `src/engine/csv/csv_tokenize.c` | Read one line (`read_next_line`), split it into fields (`next_field`) |
| CSV reader | `src/csv.c` | Header, type detection, turning lines into chunks |
| Chunk | `src/engine/chunk.h`, `src/chunk.c` | The batch of rows that moves through the engine |
| Table / printer | `src/engine/table.c` | `COLUMN`, `TABLE`, `print_table` |
| Common | `src/engine/common.c` | `BdbStatus`, `BdbError` |
| Limits | `src/engine/storage/format.h` | `BDB_VECTOR_SIZE`, `BDB_ROW_GROUP_SIZE`, CSV limits |

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
`int64_t` columns, then wrote it to `.bdb`, read it back and ran a query.
Those parts still exist but aren't called:

| Part | File | State |
|------|------|-------|
| Old `.bdb` writer | `storage/bdb.c` | int64 only, header-first format, takes a `TABLE` |
| Old `.bdb` reader | `storage/bdb_reader.c` | same |
| Query engine | `query/query.c` | scan loop commented out |
| `BdbChunkFn` | `sink/bdb_sink.h` | left over from the push design, unused |

Part 2 replaces the writer and reader. The query engine is ported after that.

---

# Part 2: .bdb v1 file format (proposed)

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
col_count        u32
for each column:
    name_len     u32
    name         name_len bytes, not null-terminated
    type         u8           enum ColumnType value
group_count      u32
for each group:
    row_count    u32          rows in this group (≤ BDB_ROW_GROUP_SIZE)
    for each column:
        offset   u64          byte offset of this column's block
```

Block sizes aren't stored, because they follow from the group's `row_count`
and the column's type (see the table above). Once blocks can be compressed,
a later version will store sizes too.

Footer size: `8 + 4 + Σ(4 + name_len + 1) + 4 + group_count × (4 + 8 × col_count)`
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

Two columns, `year` (`INT`) and `flag` (`BOOL`), 3 rows, with a group size of
2 to keep it small. Group 0 has 2 rows and group 1 has 1.

| Bytes | Contents |
|-------|----------|
| 0–3 | `"BDB1"` |
| 4–7 | version = 1 |
| **Group 0** | |
| 8–9 | `year` bitmap (2 bytes) |
| 10–25 | `year` values (2 × 8) |
| 26–27 | `flag` bitmap (2 bytes) |
| 28–29 | `flag` values (2 × 1) |
| **Group 1** | |
| 30 | `year` bitmap (1 byte) |
| 31–38 | `year` value (8) |
| 39 | `flag` bitmap (1 byte) |
| 40 | `flag` value (1) |
| **Footer** (starts at 41) | |
| 41–48 | row_count = 3 |
| 49–52 | col_count = 2 |
| 53–61 | name_len = 4, `"year"`, type = 0 |
| 62–70 | name_len = 4, `"flag"`, type = 3 |
| 71–74 | group_count = 2 |
| 75–94 | group 0: row_count = 2, offsets 8 and 26 |
| 95–114 | group 1: row_count = 1, offsets 30 and 39 |
| **Trailer** | |
| 115–122 | footer_offset = 41 |
| 123–126 | `"BDB1"` |

The file is 127 bytes. This makes a good first test file for the reader.

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

A read that ends early fails with `BDB_ERR_IO`.

## Writer

The writer is a **consumer of chunks**. It collects them into a row group
buffer and writes a group each time the buffer fills up. For a step-by-step
implementation guide with flow diagrams, see
[WRITER_FLOW.md](WRITER_FLOW.md).

```c
typedef struct {
    FILE    *file;
    CHUNK    group;          // row group buffer: capacity BDB_ROW_GROUP_SIZE
    uint64_t row_count;      // rows written so far
    uint32_t group_count;
    uint32_t *group_rows;    // row count of each group     (grows)
    uint64_t *offsets;       // group_count × col_count      (grows)
} BDB_WRITER;

BdbStatus bdb_writer_open  (BDB_WRITER *w, const char *path, BdbError *err);
BdbStatus bdb_writer_append(BDB_WRITER *w, const CHUNK *chunk, BdbError *err);
BdbStatus bdb_writer_finish(BDB_WRITER *w, BdbError *err);
void      bdb_writer_close (BDB_WRITER *w);
```

| Function | Does |
|----------|------|
| `open` | Opens the file and writes the header (`"BDB1"`, version). |
| `append` | On the first chunk, copies the schema (names, types) and allocates the group buffer. Then `memcpy`s the chunk's rows into the buffer. When the buffer holds `BDB_ROW_GROUP_SIZE` rows, writes the group: for each column, records `ftell` as its offset, then writes its bitmap and values. |
| `finish` | Writes the last, partial group if there is one, then the footer, then the trailer. |
| `close` | Frees everything and closes the file. Safe to call after an error, and safe to call twice. |

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
per column, about 1.1 MB per `INT` or `DOUBLE` column, plus 12 bytes of
metadata per group (4 for its row count, 8 for each offset). The size of the
CSV doesn't matter.

## Reader

The reader is a **source**, with the same shape as `CSV_READER`:

```c
typedef struct {
    FILE    *file;
    /* footer contents: row_count, schema, group_rows, offsets */
    uint32_t group;          // current group
    uint32_t row_in_group;   // next row to read in it
    CHUNK    chunk;          // reused, capacity BDB_VECTOR_SIZE
} BDB_READER;

BdbStatus bdb_reader_open      (BDB_READER *r, const char *path, BdbError *err);
BdbStatus bdb_reader_next_chunk(BDB_READER *r, const CHUNK **out, BdbError *err);
void      bdb_reader_close     (BDB_READER *r);
```

- `open` validates the file and loads the footer.
- `next_chunk` reads the next (up to) 2048 rows of the current group. For
  each column, it seeks to `offset + row_in_group` for the bitmap and to
  `offset + n + row_in_group × type_size` for the values. When a group runs
  out, it moves to the next group. It returns `NULL` at the end.

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
