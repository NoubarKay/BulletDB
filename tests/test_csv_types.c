//
// test_csv_types: column types from the CSV sniffer, and strict parsing of
// every value against them.
//
// Part 1 uses small files that fit inside the sniffer's sample. It checks how
// each column's type is chosen (BOOL → INT → DOUBLE, the widest type seen
// wins), that NULLs don't decide a type, that an all-NULL column is DOUBLE,
// and the files the sniffer rejects (text, bool words mixed with numbers, rows
// with the wrong number of values).
//
// Part 2 uses files longer than the sample (BDB_CSV_SAMPLE_ROWS rows). A file
// with no bad value must import every row exactly once (the reader rewinds
// after sniffing). A bad value after the sample must fail the import with its
// exact line number, instead of being truncated (2.5 → 2) or replaced
// (abc → 0, maybe → false).
//

#include "test_util.h"
#include "csv/csv_sniffer.h"

#define TYPES_CSV "types.csv"
#define TYPES_BDB "types.bdb"

// Writes `text` to TYPES_CSV and imports it into TYPES_BDB.
static BdbStatus import_text(const char *text, BdbError *err) {
    if (!write_text_file(TYPES_CSV, text)) {
        return bdb_error_set(err, BDB_ERR_IO, "could not write " TYPES_CSV);
    }
    return csv_to_bdb(TYPES_CSV, TYPES_BDB, err);
}

// Opens TYPES_BDB and reads its first chunk. Returns false, after recording a
// failure, if either step fails. The caller closes the reader either way.
static bool open_first_chunk(BDB_READER *reader, const CHUNK **chunk) {
    BdbError err = {0};
    *reader = (BDB_READER){0};
    *chunk = NULL;

    BdbStatus s = bdb_reader_open(reader, TYPES_BDB, &err);
    CHECK_STATUS(s, BDB_OK, err);
    if (s != BDB_OK) return false;

    s = bdb_reader_next_chunk(reader, chunk, &err);
    CHECK_STATUS(s, BDB_OK, err);
    CHECK(*chunk != NULL);
    return s == BDB_OK && *chunk != NULL;
}

// The import must fail with BDB_ERR_PARSE, and the message must contain
// `needle` (e.g. the line number and the column).
static void expect_parse_error(int line, BdbStatus status, const BdbError *err, const char *needle) {
    if (status != BDB_ERR_PARSE || strstr(err->message, needle) == NULL) {
        fprintf(stderr, "%s:%d: expected a parse error containing \"%s\", got %d (%s: %s)\n",
                __FILE__, line, needle, (int)status, bdb_status_str(status), err->message);
        test_failures++;
    }
}

#define EXPECT_PARSE_ERROR(status, err, needle) expect_parse_error(__LINE__, (status), &(err), (needle))

// ---------------------------------------------------------------------------
// Part 1: choosing types, inside the sample
// ---------------------------------------------------------------------------

static void test_sniffed_types(void) {
    BDB_READER reader;
    const CHUNK *chunk;

    // A decimal on row 10 makes the whole column DOUBLE; the earlier whole
    // numbers are kept exactly, and 2765.9 isn't truncated.
    {
        BdbError err = {0};
        CHECK_STATUS(import_text("id,price\n"
                                 "1,1\n2,2\n3,3\n4,4\n5,5\n6,6\n7,7\n8,8\n9,9\n"
                                 "10,2765.9\n", &err), BDB_OK, err);
        if (open_first_chunk(&reader, &chunk)) {
            CHECK(chunk->count == 10);
            CHECK(chunk->columns[0].type == BDB_COL_INT);
            CHECK(chunk->columns[1].type == BDB_COL_DOUBLE);
            const double *price = chunk->columns[1].data;
            for (int k = 0; k < 9; k++) CHECK(price[k] == (double)(k + 1));
            CHECK(price[9] == 2765.9);
        }
        bdb_reader_close(&reader);
    }

    // An empty first value doesn't decide the type: the later rows do.
    {
        BdbError err = {0};
        CHECK_STATUS(import_text("a,b\n,1\n5,2\n7,3\n", &err), BDB_OK, err);
        if (open_first_chunk(&reader, &chunk)) {
            CHECK(chunk->columns[0].type == BDB_COL_INT);
            CHECK(chunk->columns[1].type == BDB_COL_INT);      // 1 alone is BOOL, then 2 → INT
            const int64_t *a = chunk->columns[0].data;
            CHECK(chunk->columns[0].bitmap[0] == 0);
            CHECK(chunk->columns[0].bitmap[1] == 1 && a[1] == 5);
            CHECK(chunk->columns[0].bitmap[2] == 1 && a[2] == 7);
        }
        bdb_reader_close(&reader);
    }

    // A column that's NULL in every sampled row is DOUBLE.
    {
        BdbError err = {0};
        CHECK_STATUS(import_text("a,b\n,1\n,2\n", &err), BDB_OK, err);
        if (open_first_chunk(&reader, &chunk)) {
            CHECK(chunk->columns[0].type == BDB_COL_DOUBLE);
            CHECK(chunk->columns[0].bitmap[0] == 0 && chunk->columns[0].bitmap[1] == 0);
        }
        bdb_reader_close(&reader);
    }

    // Only 0 and 1: BOOL.
    {
        BdbError err = {0};
        CHECK_STATUS(import_text("f\n0\n1\n1\n", &err), BDB_OK, err);
        if (open_first_chunk(&reader, &chunk)) {
            CHECK(chunk->columns[0].type == BDB_COL_BOOL);
            const bool *f = chunk->columns[0].data;
            CHECK(!f[0] && f[1] && f[2]);
        }
        bdb_reader_close(&reader);
    }

    // Bool words, in any case, mixed with 0/1; a blank line is skipped.
    {
        BdbError err = {0};
        CHECK_STATUS(import_text("f\ntrue\nNO\n\nYes\nf\n1\n", &err), BDB_OK, err);
        if (open_first_chunk(&reader, &chunk)) {
            CHECK(chunk->count == 5);
            CHECK(chunk->columns[0].type == BDB_COL_BOOL);
            const bool *f = chunk->columns[0].data;
            CHECK(f[0] && !f[1] && f[2] && !f[3] && f[4]);
        }
        bdb_reader_close(&reader);
    }

    // 0/1 followed by a larger number: promoted to INT.
    {
        BdbError err = {0};
        CHECK_STATUS(import_text("n\n1\n0\n5\n", &err), BDB_OK, err);
        if (open_first_chunk(&reader, &chunk)) {
            CHECK(chunk->columns[0].type == BDB_COL_INT);
            const int64_t *n = chunk->columns[0].data;
            CHECK(n[0] == 1 && n[1] == 0 && n[2] == 5);
        }
        bdb_reader_close(&reader);
    }

    // The int64 extremes are INT; one past them is DOUBLE.
    {
        BdbError err = {0};
        CHECK_STATUS(import_text("v\n-9223372036854775808\n9223372036854775807\n", &err), BDB_OK, err);
        if (open_first_chunk(&reader, &chunk)) {
            CHECK(chunk->columns[0].type == BDB_COL_INT);
            const int64_t *v = chunk->columns[0].data;
            CHECK(v[0] == INT64_MIN && v[1] == INT64_MAX);
        }
        bdb_reader_close(&reader);
    }
    {
        BdbError err = {0};
        CHECK_STATUS(import_text("v\n7\n9223372036854775808\n", &err), BDB_OK, err);
        if (open_first_chunk(&reader, &chunk)) {
            CHECK(chunk->columns[0].type == BDB_COL_DOUBLE);
            const double *v = chunk->columns[0].data;
            CHECK(v[0] == 7.0 && v[1] == 9223372036854775808.0);
        }
        bdb_reader_close(&reader);
    }

    // Rejected by the sniffer.
    {
        BdbError err = {0};
        BdbStatus s = import_text("id,status\n1,Shipped\n", &err);
        EXPECT_PARSE_ERROR(s, err, "column 'status'");             // text isn't supported yet
    }
    {
        BdbError err = {0};
        BdbStatus s = import_text("id,f\n1,yes\n2,5\n", &err);
        EXPECT_PARSE_ERROR(s, err, "column 'f'");                  // yes can't become an INT
    }
    {
        BdbError err = {0};
        BdbStatus s = import_text("a,b\n1,2\n3\n", &err);
        EXPECT_PARSE_ERROR(s, err, "line 3: expected 2 values, got 1");
    }
    {
        BdbError err = {0};
        BdbStatus s = import_text("a,b\n1,2\n\n3,4,5\n", &err);
        EXPECT_PARSE_ERROR(s, err, "line 4: more values than");    // the blank line still counts
    }
}

// ---------------------------------------------------------------------------
// Part 2: files longer than the sample
// ---------------------------------------------------------------------------

#define LONG_CSV   "types_long.csv"
#define LONG_BDB   "types_long.bdb"
#define LONG_ROWS  ((uint64_t)BDB_CSV_SAMPLE_ROWS + 2000)
#define AFTER_ROW  ((uint64_t)BDB_CSV_SAMPLE_ROWS + 500)     // a data row after the sample
#define NO_ROW     UINT64_MAX

typedef enum { LONG_INT, LONG_DOUBLE, LONG_BOOL } LongKind;

// Row i's value: INT i + 2, DOUBLE "i.5", BOOL alternating true/false.
static void long_value(LongKind kind, uint64_t i, char *buf, size_t size) {
    switch (kind) {
        case LONG_INT:    snprintf(buf, size, "%" PRIu64, i + 2);                  break;
        case LONG_DOUBLE: snprintf(buf, size, "%" PRIu64 ".5", i);                 break;
        case LONG_BOOL:   snprintf(buf, size, "%s", i % 2 == 0 ? "true" : "false"); break;
    }
}

// The line a data row is on: line 1 is the header.
static uint64_t line_of(uint64_t row) { return row + 2; }

// Writes a one-column file "v" of LONG_ROWS rows of `kind`, with `replacement`
// in place of row `row`'s value (NO_ROW: none), and imports it.
static BdbStatus import_long(LongKind kind, uint64_t row, const char *replacement, BdbError *err) {
    FILE *f = fopen(LONG_CSV, "w");
    if (f == NULL) return bdb_error_set(err, BDB_ERR_IO, "could not write " LONG_CSV);

    fprintf(f, "v\n");
    for (uint64_t i = 0; i < LONG_ROWS; i++) {
        char value[64];
        if (i == row) snprintf(value, sizeof value, "%s", replacement);
        else          long_value(kind, i, value, sizeof value);
        fprintf(f, "%s\n", value);
    }
    if (fclose(f) != 0) return bdb_error_set(err, BDB_ERR_IO, "could not write " LONG_CSV);

    return csv_to_bdb(LONG_CSV, LONG_BDB, err);
}

// Imports a long file with a bad value after the sample and checks the error
// names that exact line, the value, the type and the column.
static void expect_bad_after_sample(int line, LongKind kind, const char *bad, const char *type_name) {
    BdbError err = {0};
    BdbStatus s = import_long(kind, AFTER_ROW, bad, &err);

    char needle[160];
    snprintf(needle, sizeof needle, "line %" PRIu64 ": '%s' is not a valid %s for column 'v'",
             line_of(AFTER_ROW), bad, type_name);
    expect_parse_error(line, s, &err, needle);
}

#define EXPECT_BAD_AFTER_SAMPLE(kind, bad, type_name) \
    expect_bad_after_sample(__LINE__, (kind), (bad), (type_name))

static void test_after_sample(void) {
    // No bad value: every row is imported exactly once, so the rewind after
    // sniffing neither loses nor repeats the sampled rows.
    {
        BdbError err = {0};
        CHECK_STATUS(import_long(LONG_INT, NO_ROW, NULL, &err), BDB_OK, err);

        int64_t expected_sum = 0;
        for (uint64_t i = 0; i < LONG_ROWS; i++) expected_sum += (int64_t)(i + 2);

        AggResult r = run_aggregate(LONG_BDB, BDB_AGG_COUNT, "v");
        CHECK_STATUS(r.status, BDB_OK, r.err);
        CHECK(r.i == (int64_t)LONG_ROWS);

        r = run_aggregate(LONG_BDB, BDB_AGG_SUM, "v");
        CHECK_STATUS(r.status, BDB_OK, r.err);
        CHECK(r.type == BDB_COL_INT && r.i == expected_sum);
    }

    // A decimal inside the sample but past the first chunk still makes the
    // column DOUBLE, and every value is kept.
    {
        BdbError err = {0};
        uint64_t row = 5000;
        CHECK_STATUS(import_long(LONG_INT, row, "7.25", &err), BDB_OK, err);

        double expected_sum = 0.0;
        for (uint64_t i = 0; i < LONG_ROWS; i++) expected_sum += i == row ? 7.25 : (double)(i + 2);

        AggResult r = run_aggregate(LONG_BDB, BDB_AGG_SUM, "v");
        CHECK_STATUS(r.status, BDB_OK, r.err);
        CHECK(r.type == BDB_COL_DOUBLE && r.d == expected_sum);
    }

    // NULL and other valid values after the sample are fine.
    {
        BdbError err = {0};
        CHECK_STATUS(import_long(LONG_INT, AFTER_ROW, "", &err), BDB_OK, err);
        AggResult r = run_aggregate(LONG_BDB, BDB_AGG_COUNT, "v");
        CHECK_STATUS(r.status, BDB_OK, r.err);
        CHECK(r.i == (int64_t)LONG_ROWS - 1);

        CHECK_STATUS(import_long(LONG_INT, AFTER_ROW, "-42", &err), BDB_OK, err);
        CHECK_STATUS(import_long(LONG_DOUBLE, AFTER_ROW, "3", &err), BDB_OK, err);
        CHECK_STATUS(import_long(LONG_DOUBLE, AFTER_ROW, "1e3", &err), BDB_OK, err);
        CHECK_STATUS(import_long(LONG_BOOL, AFTER_ROW, "YES", &err), BDB_OK, err);
        CHECK_STATUS(import_long(LONG_BOOL, AFTER_ROW, "0", &err), BDB_OK, err);
    }

    // Bad values after the sample: an error naming the line, never a guess.
    EXPECT_BAD_AFTER_SAMPLE(LONG_INT, "2.5", "INT");                    // was stored as 2
    EXPECT_BAD_AFTER_SAMPLE(LONG_INT, "abc", "INT");                    // was stored as 0
    EXPECT_BAD_AFTER_SAMPLE(LONG_INT, "12abc", "INT");                  // was stored as 12
    EXPECT_BAD_AFTER_SAMPLE(LONG_INT, "99999999999999999999", "INT");   // overflow, was INT64_MAX
    EXPECT_BAD_AFTER_SAMPLE(LONG_DOUBLE, "1.2.3", "DOUBLE");
    EXPECT_BAD_AFTER_SAMPLE(LONG_DOUBLE, "abc", "DOUBLE");
    EXPECT_BAD_AFTER_SAMPLE(LONG_BOOL, "maybe", "BOOL");                // was stored as false
    EXPECT_BAD_AFTER_SAMPLE(LONG_BOOL, "2", "BOOL");
}

int main(void) {
    test_sniffed_types();
    test_after_sample();
    return test_report("test_csv_types");
}
