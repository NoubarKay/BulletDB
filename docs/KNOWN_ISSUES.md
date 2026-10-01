# Known limitations and bugs

## Limitations

Things that don't exist yet, or work in a restricted way, by design for now.

- **Types come from the first data row only.** A later value that doesn't fit
  is stored wrongly instead of being rejected. For example, `abc` in an `INT`
  column becomes `0`, and a column detected as `INT` from its first value
  (`2871`) cuts later decimals (`2765.9` → `2765`).
- **Only an empty field is NULL.** Text like `NULL` or `NA` is read as a value
  (0 in a number column).
- **No strings yet.** A `STR` column fails the import.
- **Queries are hard-coded in `main.c`:** one aggregate of one column. There's
  no `WHERE`, no `GROUP BY`, and no SQL text yet.
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
- [ ] `bdb_now` (Windows) computes `counter × 1,000,000,000` before dividing,
      which overflows `int64_t` once the machine has been up for about 15
      minutes, so operator timings can be garbage. Split it into seconds and
      remainder before multiplying
- [ ] The aggregate's "invalid column type" error message passes the
      aggregate's name (a string) to a `%d`, so the message is wrong (and
      it's undefined behavior). It should be `%s`
- [ ] `csv_open` ignores the status returned by `parse_header`
- [ ] An empty value in the first data row fails the import:
      `detect_file_type("")` returns `STR`, which is rejected
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
