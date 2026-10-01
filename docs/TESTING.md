# Testing and CI

## Running the tests

The tests are in `tests/`. Each one is a small program that returns 0 when
every check passes, and CTest runs them all:

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

In CLion, the tests appear as run configurations, and **Run 'All CTest'** runs
them together.

| Test | What it checks |
|------|----------------|
| `test_format` | A one-row CSV becomes exactly the 193-byte `.bdb` file from the [worked example](DESIGN.md#worked-example), byte for byte |
| `test_roundtrip` | 246,760 generated rows (3 row groups, NULLs in three columns) go CSV → `.bdb` → reader and come back with every value and every NULL in place, in the right number of chunks; aggregates over the whole file match totals computed while generating |
| `test_aggregates` | `SUM`, `COUNT`, `MIN`, `MAX` and `AVG` on a small file with known answers: NULLs skipped, result types (`COUNT` is INT, `AVG` is DOUBLE), result names, and a missing column |
| `test_corrupt` | Damaged files (wrong magic, unknown version, cut off, footer offset past the end, empty, too small) are rejected with `BDB_ERR_FORMAT`, never a crash |
| `test_filter` | `FILTER` with selection vectors: all six comparisons on INT and DOUBLE columns, NULL never matching, complementary filters adding up, aggregates seeing only selected rows, two stacked filters, a filter matching nothing, error cases, and filters across 2 row groups where most chunks have no match |

The tests write their CSV and `.bdb` files into the build directory, so they
don't depend on any data file in the repository. Shared helpers (`CHECK`,
`REQUIRE`, CSV → `.bdb`, running one aggregate) are in `tests/test_util.h`.

**When the file format changes** (for example format v2 with per-group
statistics), `test_format`'s expected bytes and the worked example in
DESIGN.md must be updated together.

## Sanitizers

On Linux or macOS, build with AddressSanitizer and UndefinedBehaviorSanitizer
to catch memory errors, leaks and undefined behavior (they don't work with
MinGW on Windows):

```sh
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DBDB_SANITIZE=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

## Warnings

Every target is built with `-Wall -Wextra`. A missing `return` in a non-`void`
function, and calling an undeclared function, are build errors.

## Continuous integration

Four workflows run on GitHub Actions:

| Workflow | When | What |
|----------|------|------|
| [`ci.yml`](../.github/workflows/ci.yml) | pushes and pull requests to `main`, `v*` tags | build + tests on three platforms (below) |
| [`codeql.yml`](../.github/workflows/codeql.yml) | pushes and pull requests to `main`, and weekly | GitHub's static analysis for C, looking for memory and security bugs |
| [`release.yml`](../.github/workflows/release.yml) | a `v*` tag is pushed | tests again, builds Linux and Windows binaries, and publishes a GitHub Release with the notes from the [changelog](../CHANGELOG.md) |
| [`dependabot.yml`](../.github/dependabot.yml) | weekly | opens pull requests when the GitHub Actions used by these workflows release new versions |

### `ci.yml`

Three jobs:

| Job | Platform | Purpose |
|-----|----------|---------|
| Linux (GCC, Release) | Ubuntu 24.04 | The main build and test run |
| Linux (GCC, AddressSanitizer + UBSan) | Ubuntu 24.04 | Fails on any memory error, leak or undefined behavior |
| Windows (MinGW-w64, Release) | Windows | The same kind of toolchain as CLion on Windows |

Inside each job, every test area is its **own step**, with its own ✓ / ✗ /
skipped: *format*, *round trip*, *aggregates*, *corrupt files* and *filter*.
All of them run even when one fails.

| Event | Runs |
|-------|------|
| Push to `main`, a `v*` tag, or a manual run | **Everything**, always |
| Pull request to `main` | Only what the changed files affect (below) |
| Pull request changing only docs (`docs/`, `*.md`, images) | Nothing: the three build jobs are skipped |

For pull requests, a first job (*Decide what to test*) maps the changed files
to the tests that exercise them:

| Changed files | Tests run |
|---------------|-----------|
| `src/common`, `src/core`, `src/csv`, the writer, `bdb_format.h`, `io`, `memory`, any `CMakeLists.txt`, `tests/test_util.h`, the workflow | all five |
| `src/storage/bdb_reader.*` | round trip, aggregates, corrupt, filter |
| `src/executor/**` | round trip, aggregates, filter |
| `tests/test_<name>.c` | that test |
| `main.c`, `src/storage/debug.*` | none (build only) |

To make the checks required before merging, enable branch protection for
`main` (**Settings → Branches**) and mark the three build jobs as required. A
skipped job counts as passing, so docs-only pull requests can still be merged.

**Adding a test:** add it to `tests/CMakeLists.txt`, add a step for it to each
job in `ci.yml`, and map it to the source folders it exercises in the *Decide
what to test* job.

### Releasing

1. Move the changes under **Unreleased** in [CHANGELOG.md](../CHANGELOG.md)
   into a new section, for example `## [0.2.0] - 2026-10-15`, and commit.
2. Tag and push:

   ```sh
   git tag -a v0.2.0 -m "Release 0.2.0"
   git push origin v0.2.0
   ```

3. `release.yml` runs the tests, builds the binaries, and creates the release
   with that changelog section as its notes. If any test fails, nothing is
   published.
