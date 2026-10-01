# Changelog

All notable changes to BulletDB are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). Before 1.0, any release may change
the file format or the API.

When releasing, move the **Unreleased** entries into a new version section.
The [release workflow](.github/workflows/release.yml) uses that section as the
GitHub Release notes.

## [Unreleased]

Work towards 0.2: `FILTER` with selection vectors, per-group statistics
(format v2) and zone-map pruning.

### Added
- Test suite (CTest): `test_format` (the 193-byte worked example, byte for
  byte), `test_roundtrip` (246,760 rows across 3 row groups, every value and
  NULL checked), `test_aggregates` (all five aggregates with known answers),
  and `test_corrupt` (damaged files are rejected, never a crash).
- GitHub Actions: CI on Linux, Linux with AddressSanitizer + UBSan, and
  Windows (MinGW-w64), with one step per test area and per-area test
  selection on pull requests; CodeQL analysis; a release workflow that builds
  Linux and Windows binaries; Dependabot for workflow updates.
- CMake options `BDB_BUILD_TESTS` and `BDB_SANITIZE`, and `-Wall -Wextra`
  on every target, with a missing `return` as a build error.
- Logo (light and dark versions) and status badges in the README.
- `CHANGELOG.md`.

### Changed
- The engine is now built as a static library, `bulletdb`, linked by the
  program and the tests.
- Minimum CMake version lowered from 4.3 to 3.21.
- The README is slimmed down to an overview; details moved to `docs/`
  (`USAGE.md`, `TESTING.md`, `PERFORMANCE.md`, `KNOWN_ISSUES.md`,
  `ROADMAP.md`).

### Fixed
- Aggregate result types and names (`COUNT` is INT, `AVG` is DOUBLE).

### Removed
- Sample data (`sales.csv`, `test.bdb`) and IDE settings (`.idea/`) from the
  repository; tests generate their own data. The scratch file `test.c`.

## [0.1] - 2026-09-30

First tagged snapshot: a working columnar storage engine with a small
vectorized query executor.

### Added
- **CSV import** that streams a file as chunks of up to 2048 typed rows
  (`INT`, `DOUBLE`, `BOOL`), with types detected from the first data row and
  empty fields stored as NULL. Fields are split in place, without allocating.
- **`.bdb` v1 columnar file format:** row groups of up to 122,880 rows, one
  contiguous block per column (NULL bitmap, then values), and a footer with
  the schema, each group's row count and the offset of every column block.
- **Writer** (`BDB_WRITER`) that streams chunks into row groups with fixed
  memory, whatever the input size.
- **Reader** (`BDB_READER`) that loads only the header and footer, then
  streams the data back one chunk at a time.
- **Vectorized query executor** (Volcano model, one chunk per `next`):
  `SCAN` and `AGGREGATE` operators, with `SUM`, `COUNT`, `MIN`, `MAX` and
  `AVG` following SQL's NULL rules.
- **`bdb_explain`** plan printer, with per-operator statistics: `next` calls,
  chunks, rows and time.
- Portable 64-bit file I/O (`bdb_fseek`, `bdb_ftell`) and a monotonic clock
  (`bdb_now`) for Windows and POSIX.
- Debug dumps for the writer and reader that print a file's layout and check
  it for consistency.
- Design documentation (`docs/DESIGN.md`) with the full file format
  specification and a byte-by-byte worked example.

[Unreleased]: https://github.com/NoubarKay/BulletDB/compare/v0.1...HEAD
[0.1]: https://github.com/NoubarKay/BulletDB/releases/tag/v0.1
