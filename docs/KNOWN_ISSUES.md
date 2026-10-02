# Known limitations and bugs

## Limitations

Things that don't exist yet, or work in a restricted way, by design for now.

- **Types come from a sample of 30,720 rows.** A value after the sample that
  doesn't fit its column (a first decimal in an `INT` column, say) stops the
  import with an error naming the line. There's no way yet to declare a
  column's type, or to skip bad rows, to get past it.
- **A column of only `0` and `1` in the sample is `BOOL`,** so `SUM`/`AVG`
  reject it, and a later `2` is an error.
- **CSV input must be a seekable file.** The reader seeks back after
  sniffing, so a pipe or stdin won't work.
- **Only an empty field is NULL.** Text like `NULL` or `NA` is read as text,
  so in a number column it fails the import.
- **No strings yet.** A `STR` column fails the import.
- **Queries are hard-coded in `main.c`:** operator trees built in C. There's
  no `GROUP BY` and no SQL text yet.
- **`FILTER` is one comparison of a column against a number.** Only `INT`
  and `DOUBLE` columns; no `OR`, no column-to-column comparisons. Several
  conditions combine with `AND` by stacking filters.
- **Filtered chunks can't be written to a `.bdb` file.** `bdb_writer_append`
  rejects a chunk with a selection vector rather than copying the selected
  rows.
- **Printed row numbers restart in every chunk.** A chunk doesn't know where
  it sits in the file.
- **Aggregates only work on `INT` and `DOUBLE` columns.** A `BOOL` column is
  rejected, even for `COUNT`.
- **The reader always reads every column.** Reading only the columns a query
  needs (projection) is planned; the format already supports it.
- **CSV:** quoted fields and commas inside values aren't supported.
- **Fixed output file.** `main` always writes `test-1.bdb`.
- **File format:** little-endian only. No per-group statistics or compression
  yet (planned for format v2).
- **Blocks aren't aligned.** Values can start at odd byte offsets in the file.
  That's fine for `fread`, but reading values in place from a memory-mapped
  file would need blocks padded to 8 bytes (planned for format v2).

## Bugs

Things that are broken right now, most serious first.

**Crashes or wrong data**
- [ ] `FILTER` with a fractional constant on an `INT` column drops the
      fraction: `qty = 50.5` behaves like `qty = 50`, and `qty >= 50.5` like
      `qty >= 50`. Fix by adjusting the comparison once per query (`=` →
      no match, `!=` → every non-NULL row, `<`/`<=` → `<= floor(x)`,
      `>`/`>=` → `>= ceil(x)`)
- [ ] `FILTER` parses its constant as a `double`, so integer constants above
      2⁵³ (about 9 × 10¹⁵) lose precision. Parse whole-number constants with
      `strtoll` for `INT` columns
- [ ] The CSV reader accepts `nan` and `inf` as `DOUBLE` values (`strtod`
      parses them), and the writer doesn't skip `NaN` when it computes a row
      group's min and max. If a group's first non-NULL value is `NaN`, both
      stats are `NaN` for that group, because every comparison with `NaN` is
      false. Harmless until zone-map pruning uses the stats; then it can skip
      a group that has matching rows. Fix by rejecting `nan`/`inf` at import
      (which also keeps them out of `SUM` and `AVG`), or by skipping `NaN` in
      `compute_stats`
- [ ] `bdb_now` (Windows) computes `counter × 1,000,000,000` before dividing,
      which overflows `int64_t` once the machine has been up for about 15
      minutes, so operator timings can be garbage. Split it into seconds and
      remainder before multiplying
- [ ] The aggregate's "invalid column type" error message passes the
      aggregate's name (a string) to a `%d`, so the message is wrong (and
      it's undefined behavior). It should be `%s`
- [ ] A CSV with a header but no data rows writes a `.bdb` footer with 0
      columns: the writer only learns the schema from the first chunk
- [ ] `bdb_write_group` doesn't check `bdb_ftell` for failure (-1) before
      storing it as an offset
- [ ] `bdb_reader_open` doesn't yet reject a bad `footer_offset`, a
      `col_count` outside 1..100, bad group row counts, or a footer that
      doesn't end at the trailer (these are only checked by
      `bdb_reader_debug_dump`). A corrupted footer could make `next_chunk`
      read from the wrong place. It also doesn't check the `name_len` read or
      the `malloc` results for `row_groups` and `offsets`

**Memory**
- [ ] `free_table` doesn't free `bitmap`
- [ ] If `csv_open` fails, `main` returns without calling `csv_close`, so the
      file and buffer leak

**Cleanup**
- [ ] `storage/io.h` includes `<windows.h>`, and through `bdb_operator.h` that
      reaches every operator. Defining `WIN32_LEAN_AND_MEAN` and `NOMINMAX`
      first (or moving `bdb_now` into `io.c`) avoids its `min`/`max`/`BOOL`
      macros
- [ ] `chunk_init` takes a `col_count` it doesn't need
- [ ] The `memcpy` in `bdb_writer_append` adds an offset to a `void *`. GCC
      allows this as an extension, but standard C needs a `(char *)` cast
- [ ] `parse_header` still splits with `strtok`, so column names aren't
      trimmed and an empty name (`year,,price`) is skipped instead of
      reported. It could use `next_field` like the rows
- [ ] In `BdbAggregate`, the aggregate kind is stored in a field called
      `type`, next to `col_type` for the column's type. The similar names
      already caused one bug; renaming it to `kind` would prevent more
- [ ] With warnings enabled, the existing code triggers some on Linux, for
      example `%llu` used for `uint64_t` values (correct on Windows, a format
      warning on Linux, where it should be `PRIu64`)
