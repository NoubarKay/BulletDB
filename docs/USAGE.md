# Using BulletDB

How to run the program, what it prints, the CSV format it accepts, and the
debugging tools. For how the engine works inside, see [DESIGN.md](DESIGN.md).

## Contents

- [Running it](#running-it)
- [Changing the query](#changing-the-query)
- [Printing chunks](#printing-chunks)
- [Debug dumps](#debug-dumps)
- [Exit codes](#exit-codes)
- [CSV input](#csv-input)
- [Error handling](#error-handling)

## Running it

```sh
BulletDB <file.csv>
```

One run does two things:

1. **Import:** reads the CSV one chunk at a time and writes every chunk to
   **`test-1.bdb`** in the current directory.
2. **Query:** builds a small operator tree over `test-1.bdb`, runs it, prints
   the result, and then prints the plan with per-operator statistics.

```text
+----------------+
| SUM(PRICEEACH) |
| double         |
+----------------+
|    18883385.22 |
+----------------+
(1 rows, 1 columns)
AGGREGATE SUM(PRICEEACH) calls: 2  chunks: 1  rows: 1  time: ... ms
        +-SCAN (225721 rows, 5 columns, 2 groups)
calls: 112  chunks: 111  rows: 225721  time: ... ms
```

The table is the **result**: a chunk with one column and one row. The lines
after it are the **plan** (`bdb_explain`), with how many times each operator's
`next` was called, how many chunks and rows it returned, and the time spent in
it.

Relative paths are resolved from the current working directory. In CLion, set
**Run → Edit Configurations… → Working directory** to `$ProjectFileDir$` and
put the CSV name in **Program arguments**.

## Changing the query

The query is hard-coded in `main.c`, in the `bdb_aggregate_init` call:

```c
bdb_aggregate_init(&agg, &scan.base, BDB_AGG_COUNT, "PRICEEACH");
//                                   ^ SUM, COUNT, MIN, MAX or AVG   ^ column name
```

| Aggregate | Result | Result type | With no non-NULL values |
|-----------|--------|-------------|-------------------------|
| `SUM` | total of the non-NULL values | the column's type | NULL |
| `COUNT` | number of non-NULL values | always INT | 0 |
| `MIN` | smallest value | the column's type | NULL |
| `MAX` | largest value | the column's type | NULL |
| `AVG` | total ÷ count | always DOUBLE | NULL |

NULL values are skipped by all of them, as in SQL. They work on `INT` and
`DOUBLE` columns. A column name that doesn't exist fails with
`Column X not found`.

## Printing chunks

`print_chunk_cb` in `main.c` prints any chunk as a table. In the import loop it
shows the CSV data:

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

The printer (`print_table` in `core/table.c`) sizes each column to its longest
name, type or value, right-aligns numbers, prints `NULL` for missing values and
`true`/`false` for BOOL, and shows doubles with two decimals. A table longer
than 20 rows shows only the first and last 10.

A chunk holds up to `BDB_VECTOR_SIZE` rows (2048, set in `src/core/chunk.h`).
To see several chunks from a small CSV, lower it to something like 10.

## Debug dumps

Two functions in `src/storage/debug.c` print a structured summary of a file's
layout and check it:

- `bdb_writer_debug_dump(&writer, stdout)`: after `bdb_writer_finish`, before
  `bdb_writer_close`
- `bdb_reader_debug_dump(&reader, stdout)`: after `bdb_reader_open`. Returns
  the number of problems found.

For the same file, the two should match line for line:

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
  data ends at 10157453 (should equal the trailer's footer_offset)
== 0 problems ==
```

Any inconsistency is printed as a line starting with `!!`. To look at the
bytes themselves: `Format-Hex test-1.bdb` in PowerShell, or `xxd test-1.bdb`
on Linux and macOS.

## Exit codes

| Code | Meaning |
|------|---------|
| `0`  | Success |
| `1`  | Bad arguments, or an import, write or query error |

## CSV input

```csv
year,price,expensive
2023,20.00,true
2024,,true
```

Rules:
- The first line is the header and holds the column names.
- Each column's type is chosen from a **sample of the first 30,720 data
  rows**: the narrowest type that fits every non-empty value in it.

  | Values in the sample | Type | Stored as |
  |----------------------|------|-----------|
  | Only `true` `false` `t` `f` `yes` `no` `1` `0` (any case) | `BOOL` | `bool` |
  | Whole numbers, such as `2023` | `INT` | `int64_t` |
  | Any decimal, such as `20.00`, or an integer too big for `int64_t` | `DOUBLE` | `double` |
  | Empty in every sampled row | `DOUBLE` | `double` |
  | Any other text, or bool words mixed with numbers | `STR` | not supported yet, so the import fails |

  A column of `1, 2, 2765.9` is `DOUBLE`, wherever in the sample the decimal
  appears.
- Every value after the sample must fit its column's type. One that doesn't
  (`2.5` in an `INT` column, `maybe` in a `BOOL` column, an integer that
  overflows `int64_t`) stops the import with an error naming the line. It's
  never truncated or replaced.

- An empty field (`2024,,true`) is stored as **NULL** in every column type.
  Only an empty field is NULL: text like `NULL` or `NA` is read as text, so
  in a number column it fails the import.
- Spaces around a value are ignored (`2023, 20.00 ,true` works).
- Each row must have exactly one value per column. A trailing comma
  (`2025,50.00,true,`) counts as an extra, empty value, so it's an error.
- Blank lines are skipped. Both `\n` and `\r\n` line endings work.
- A line longer than `BDB_CSV_MAX_LINE` (1 MiB) is rejected, and so is a field
  longer than `BDB_CSV_MAX_FIELD` (255 characters).

Errors name the line:

```text
error: line 7: expected 3 values, got 2
error: line 9: more values than the 3 columns in the header
error: line 12: value in column 'price' is longer than 255 characters
error: line 31222: '2.5' is not a valid INT for column 'year'
error: column 'status' contains text; text columns aren't supported yet
```

How lines are split into fields, in place and without allocating, is described
in [DESIGN.md](DESIGN.md#splitting-fields).

## Error handling

Functions that can fail return a `BdbStatus` and fill in a `BdbError`
containing a readable message:

```c
BdbError err = {0};
BdbStatus status = csv_open(&reader, "data.csv", &err);
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
| `BDB_ERR_INVALID` | An invalid argument, such as an aggregate on an unsupported column type |

`bdb_status_str()` returns a short generic description of any status code.
