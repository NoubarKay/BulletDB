# Performance

Measurements so far, what they show, and the next speedups.

## CSV field splitting

CSV reader timings, per 2048-row chunk of a 3-column file (`INT`, `DOUBLE`,
`BOOL`), measured around each `csv_next_chunk` call:

| Version | Per chunk | Per row | Rows per second |
|---------|-----------|---------|-----------------|
| Copying each field (`extract_value`: 2 × `malloc` + `free` per field) | ~1.67 ms | ~820 ns | ~1.2 M |
| **Splitting fields in place** (`next_field`) | **~0.91 ms** | **~445 ns** | **~2.2 M** |

Splitting in place made the reader about **1.8 times faster**. It also fixed a
leak of one allocation per field.

## Full import benchmark

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
  of the parse time (one `DOUBLE` per row). `strtoll` costs about 10 ns, and a
  hand-written integer loop about 1.5 ns.
- `-O0` and `-O2` parse at the same speed, because the time is spent inside
  the C library, not in BulletDB's code.
- The `.bdb` file is **about 1.9 times the size of the CSV**: every integer
  takes 8 bytes, and every value has a bitmap byte. Compression and narrower
  integer types would fix this (see [DESIGN.md, Later](DESIGN.md#later)).

## Next speedups, biggest payoff first

1. **A fast path for decimals** instead of `strtod`: parse `39.61` as the
   integer 3961 and divide by 10², which gives exactly the same result as
   `strtod` for up to 15 significant digits and small exponents, and fall back
   to `strtod` otherwise. Expected: parsing about 2.5–3 times faster.
2. **A dedicated integer parser** instead of `strtoll`.
3. **Cheaper checks.** Have `next_field` return the field's length instead of
   calling `strlen` again for the max-field check, and check the first
   character before calling `strcasecmp` for BOOL.
4. **Reading in large blocks** (`fread` plus `memchr`) instead of calling
   `fgets` once per line. `fgets` locks the `FILE` on every call, and
   `read_next_line` scans each line with `strlen` twice.
5. **Projection:** the reader reads only the columns a query needs. For one
   column of five, that's about 5 times fewer bytes read.
6. Later: SIMD scanning, and parsing on several threads.

## How to measure

Time each `csv_next_chunk` (or `next`) call with a monotonic clock (`bdb_now`
in `storage/io.h`), and turn off printing. Use a Release build and a large
file, and don't read much into the first chunk or a single slow chunk. The
executor's per-operator statistics (`bdb_explain` after a run) already show
the time spent in each operator.
