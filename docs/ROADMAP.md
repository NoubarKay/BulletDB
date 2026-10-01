# Roadmap and progress

What's done, what's being worked on, and what's planned. Released versions
are listed in the [changelog](../CHANGELOG.md).

## Now: the query executor

Built one operator at a time (design in
[DESIGN.md, Part 3](DESIGN.md#part-3-query-execution)):

- [x] `BdbOperator` base struct
- [x] `SCAN` operator (owns a `BDB_READER`)
- [x] `AGGREGATE` with `SUM`, `COUNT`, `MIN`, `MAX`, `AVG`, and SQL's NULL rules
- [x] `bdb_explain`: print the operator tree
- [x] Per-operator statistics: `next` calls, chunks, rows and time (`bdb_op_next`)
- [x] Test suite and CI
- [ ] **`FILTER`** (`WHERE column op value`), with **selection vectors**:
  - [ ] 5a. `sel` in `CHUNK`; the aggregate follows it (results unchanged)
  - [ ] 5b. the filter struct (column, comparison, value, its own `sel` buffer)
  - [ ] 5c. `filter_next`: build `sel` from the child's chunk, skipping NULLs
  - [ ] 5d. `print_table` follows `sel`; the writer rejects selected chunks
  - [ ] 5e. `test_filter`
- [ ] Tracing: print the rows leaving a chosen operator

## Next: format v2 and pruning

- [ ] Per-row-group statistics in the footer: `min`, `max`, `null_count` per
      column (format v2)
- [ ] Zone-map pruning: the scan skips row groups whose min/max can't match the
      filter (predicate pushdown)
- [ ] `MIN`, `MAX`, `COUNT` without `WHERE` answered from the footer alone
- [ ] Projection: the reader reads only the columns a query needs

## Later

- [ ] Multiple filter conditions (`AND` / `OR`)
- [ ] `GROUP BY`
- [ ] CSV reader speedups (see [PERFORMANCE.md](PERFORMANCE.md))
- [ ] Inner loops the compiler can turn into SIMD instructions
- [ ] Pack NULL bitmaps into 1 bit per row, stored as `uint64_t` words
      (32 words per 2048-row chunk)
- [ ] Strings, stored with a dictionary
- [ ] Narrower integer storage, chosen per row group by the writer from the
      group's min and max (`INT` stays `int64_t` in memory and in queries),
      then column compression (run-length, delta, dictionary,
      frame-of-reference + bit-packing)
- [ ] CSV: an optional user-supplied schema, and an option to skip or log
      rows that don't fit their column's type
- [ ] Importing from SQL databases (SQLite, then PostgreSQL) as another chunk
      source
- [ ] Command-line subcommands such as `import`, `query` and `info`
- [ ] A small query language

## Done

- [x] CSV import with line-level error messages
- [x] CSV type sniffing over a 30,720-row sample (`BOOL` → `INT` →
      `DOUBLE`), and strict validation of every value against its column
      type (no silent truncation, invalid `BOOL`s or integer overflow)
- [x] Typed columns (`INT`, `DOUBLE`, `BOOL`) with NULL bitmaps
- [x] Pull-based CSV reader that streams fixed-size chunks
- [x] In-place CSV field splitting (1.8 times faster, no allocations per field)
- [x] Bordered table printer
- [x] Columnar `.bdb` v1 file writer: typed column blocks with NULL bitmaps,
      row groups, and a footer with the schema and every block's offset
- [x] `.bdb` reader: opens a file, loads its footer, and streams the data back
      one chunk at a time
- [x] Portable 64-bit file I/O (`bdb_fseek` / `bdb_ftell`) on Windows and POSIX

## Milestones

**Storage round trip (2026-09-29).** CSV → chunks → `.bdb` → chunks works end
to end. A 225,721-row, 5-column CSV is written as 2 row groups and read back
as 111 chunks with every row accounted for, and every column's `SUM` matches
the CSV exactly, while the reader holds only the footer and one 2048-row
chunk in memory.

**First query through an operator tree (2026-09-29).** `SUM(PRICEEACH)` runs
as `AGGREGATE → SCAN` on the stored file and returns 18883385.22, the same
value as the CSV.

**All five basic aggregates (2026-09-30).** `SUM`, `COUNT`, `MIN`, `MAX` and
`AVG` run on `INT` and `DOUBLE` columns, skip NULLs, and return NULL (or 0 for
`COUNT`) when there are no values.

**Tests and CI (2026-10-01).** Four CTest tests (format, round trip,
aggregates, corrupt files), run on Linux, Linux with sanitizers, and Windows
on every push.

Expected sums for the 225,721-row sample file used during development:

| Column | SUM |
|--------|-----|
| ORDERNUMBER | 2315614239 |
| QUANTITYORDERED | 7921042 |
| PRICEEACH | 18883385.22 |
| ORDERLINENUMBER | 1459774 |
| SALES | 802153919 |
