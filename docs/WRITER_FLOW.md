# Writer implementation flow

A step-by-step guide for writing the chunk-writing part of `BDB_WRITER`
(`src/storage/bdb_writer.c`): `append`, `init_group`, `write_group` and a
minimal `finish`. It covers writing row groups only. The footer and trailer
come later.

For the file format itself and the reasons behind it, see
[DESIGN.md, Part 2](DESIGN.md#part-2-bdb-v1-file-format-proposed).

## Contents

0. [The big picture](#0-the-big-picture-who-calls-what)
1. [bdb_writer_append](#1-bdb_writer_appendwriter-chunk-err)
2. [init_group](#2-init_groupwriter-chunk-err-which-runs-once)
3. [write_group](#3-write_groupwriter-err-a-static-helper)
4. [What the copy does in memory](#4-what-the-copy-does-in-memory)
5. [bdb_writer_finish](#5-bdb_writer_finishwriter-err-the-minimal-version-for-now)
6. [main](#6-main)
7. [Checklist](#7-checklist-before-you-run-it)
8. [Checking the result](#8-how-to-check-it-worked)

## 0. The big picture: who calls what

```
main
 │
 ├─ bdb_writer_open ───────────▶ file: [BDB1][01 00 00 00]
 │
 ├─ loop: csv_next_chunk ──▶ chunk
 │          │
 │          └─▶ bdb_writer_append(writer, chunk)
 │                 │
 │                 ├─ (first time) init_group
 │                 ├─ copy chunk → group buffer
 │                 └─ (buffer full) write_group ──▶ file: [group bytes]
 │
 ├─ bdb_writer_finish
 │        └─ (rows left in buffer) write_group ──▶ file: [last group bytes]
 │
 └─ bdb_writer_close   (free memory, fclose)
```

## 1. `bdb_writer_append(writer, chunk, err)`

```
                    ┌──────────────────────────┐
                    │ bdb_writer_append        │
                    └────────────┬─────────────┘
                                 ▼
                    ┌──────────────────────────┐
                    │ writer->have_schema?     │
                    └──────┬────────────┬──────┘
                        no │            │ yes
                           ▼            │
              ┌─────────────────────┐   │
              │ init_group(writer,  │   │
              │            chunk)   │   │
              └──────────┬──────────┘   │
                  failed?│──yes──▶ return error
                         │ no           │
                         ▼              │
                         ├◀─────────────┘
                         ▼
          ┌──────────────────────────────────┐
          │ group.count + chunk->count       │
          │        > BDB_ROW_GROUP_SIZE ?    │──yes──▶ return BDB_ERR_INVALID
          └────────────────┬─────────────────┘
                           │ no
                           ▼
          ┌──────────────────────────────────┐
          │ for each column c:               │
          │   size = type size of column c   │
          │   copy chunk values  → group     │  (see section 4)
          │   copy chunk bitmap  → group     │
          └────────────────┬─────────────────┘
                           ▼
          ┌──────────────────────────────────┐
          │ group.count += chunk->count      │
          └────────────────┬─────────────────┘
                           ▼
          ┌──────────────────────────────────┐
          │ group.count == ROW_GROUP_SIZE ?  │
          └──────┬──────────────────┬────────┘
             yes │                  │ no
                 ▼                  ▼
        ┌─────────────────┐   ┌───────────┐
        │ write_group     │   │ return OK │
        │ return its      │   └───────────┘
        │ status          │
        └─────────────────┘
```

**Why a chunk always fits:** a row group is exactly 60 chunks, every chunk
except the last is full (2,048 rows), and the partial last chunk arrives at
the very end. So the buffer fills up exactly and never overflows. The
`> BDB_ROW_GROUP_SIZE` check is a guard in case that assumption ever breaks.

## 2. `init_group(writer, chunk, err)`, which runs once

```
┌─────────────────────────────────────────────┐
│ group.columns = calloc(col_count, COLUMN)   │──NULL?──▶ return NOMEM
│ group.col_count = chunk->col_count          │
│ group.count = 0                             │
└───────────────────────┬─────────────────────┘
                        ▼
      ┌───────── for each column c ─────────┐
      │                                      │
      │  dst.type   = src.type               │
      │  dst.name   = strdup(src.name)       │
      │  dst.data   = calloc(ROW_GROUP_SIZE, │
      │                      type size)      │
      │  dst.bitmap = calloc(ROW_GROUP_SIZE, │
      │                      1)              │
      │                                      │
      │  any NULL? ──yes──▶ return NOMEM     │
      │                (close will clean up) │
      └───────────────────┬──────────────────┘
                          ▼
             ┌────────────────────────┐
             │ have_schema = true     │
             │ return OK              │
             └────────────────────────┘
```

**Why `calloc` for `columns`:** every pointer starts out `NULL`, so if you
fail halfway through, `chunk_free` in `bdb_writer_close` can still free
everything safely.

## 3. `write_group(writer, err)`, a `static` helper

```
      ┌───────── for each column c ─────────┐
      │                                      │
      │  size = type size of column c        │
      │                                      │
      │  (later: record ftell(file) here as  │
      │   this column's offset, for the      │
      │   footer)                            │
      │                                      │
      │  fwrite bitmap:                      │
      │     group.count items of 1 byte      │──short?──▶ return IO error
      │                                      │
      │  fwrite values:                      │
      │     group.count items of size bytes  │──short?──▶ return IO error
      │                                      │
      └───────────────────┬──────────────────┘
                          ▼
             ┌────────────────────────┐
             │ chunk_reset(&group)    │   count = 0, buffers kept
             │ return OK              │
             └────────────────────────┘
```

The order is **bitmap first, then values, one column at a time**. That's the
column block layout in DESIGN.md.

## 4. What the copy does in memory

```
chunk (from the CSV reader)          group buffer (writer)
count = 2048                         count = 4096  (before the copy)

price.data                           price.data
┌──────────────┐                     ┌──────────────┬──────────────┬─────────
│ 2048 doubles │ ──── memcpy ──────▶ │ rows 0..4095 │ rows 4096..  │  free
└──────────────┘                     └──────────────┴──────────────┴─────────
                                                    ▲
                        destination = data + (group.count × size)   ← in BYTES
                        length      = chunk->count × size           ← in BYTES

price.bitmap                         price.bitmap
┌──────────────┐                     ┌──────────────┬──────────────┬─────────
│ 2048 bytes   │ ──── memcpy ──────▶ │ rows 0..4095 │ rows 4096..  │  free
└──────────────┘                     └──────────────┴──────────────┴─────────
                                                    ▲
                        destination = bitmap + group.count
                        length      = chunk->count
```

**Watch out:** `data` is a `void *`, and you can't add to a `void *`. Cast it
to `char *` first so the offset counts **bytes**. Then multiply by `size`.
The bitmap is already `char *`, so it needs no cast and no multiplication.

## 5. `bdb_writer_finish(writer, err)`, the minimal version for now

```
┌────────────────────────┐
│ group.count > 0 ?      │
└────┬──────────────┬────┘
 yes │              │ no
     ▼              ▼
┌──────────────┐ ┌───────────┐
│ write_group  │ │ return OK │
│ return it    │ └───────────┘
└──────────────┘
      (later: footer + trailer go here)
```

Without `finish`, a file smaller than one row group writes nothing at all,
because the buffer never fills up.

## 6. `main`

```
csv_open ──fail──▶ print error, return 1
   │
bdb_writer_open ──fail──▶ print error, csv_close, return 1
   │
   ▼
┌──▶ csv_next_chunk ──error──────────────┐
│        │                                │
│     chunk == NULL? ──yes──┐             │
│        │ no               │             │
│        ▼                  │             │
│  bdb_writer_append        │             │
│        │                  │             │
│     error? ──yes──────────┼─────────────┤
│        │ no               │             │
└────────┘                  ▼             │
                  bdb_writer_finish       │
                        │                 │
                     error? ──yes─────────┤
                        │ no              ▼
                        ▼          print error
                        ├◀────────────────┘
                        ▼
            bdb_writer_close
            csv_close
            return status == OK ? 0 : 1
```

## 7. Checklist before you run it

```
[ ] bdb_writer.h   declares bdb_writer_append and bdb_writer_finish
[ ] bdb_writer.c   includes <stdlib.h> (calloc) and <string.h> (memcpy, strdup)
[ ] init_group and write_group are static (private to bdb_writer.c)
[ ] BDB_ROW_GROUP_SIZE temporarily (2 * BDB_VECTOR_SIZE) in core/chunk.h,
    so both "group full" and "last partial group" get exercised
[ ] main calls append in the loop, then finish, then close
```

Set `BDB_ROW_GROUP_SIZE` back to `(60 * BDB_VECTOR_SIZE)` when you're done
testing.

## 8. How to check it worked

```
expected size = 8 (header) + rows × bytes per row

bytes per row:  year      (INT)    1 + 8 =  9
                price     (DOUBLE) 1 + 8 =  9
                expensive (BOOL)   1 + 1 =  2
                                           ───
                                            20

10,000 rows → 8 + 200,000 = 200,008 bytes
```

```powershell
(Get-Item test.bdb).Length                  # 200008
Format-Hex test.bdb | Select-Object -First 2
# 42 44 42 31 01 00 00 00 01 01 01 01 ...
#  B  D  B  1  version=1   year bitmap starts
```

| If you see | Probably |
|---|---|
| 8 bytes | `finish` isn't called, or the group size is still 122,880 |
| Less than 200,008 | The last group isn't written, or `count` isn't reset after a write |
| More than 200,008 | `count` isn't reset, so rows are written twice |
| A crash in `memcpy` | The destination isn't cast to `char *`, or the offset isn't multiplied by `size` |

The numbers above are for the 3-column `sales.csv`. For another file, add up
`1 + type size` for each column to get the bytes per row.
