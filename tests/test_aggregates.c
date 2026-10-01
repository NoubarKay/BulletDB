//
// test_aggregates: SUM, COUNT, MIN, MAX and AVG on a small file whose answers
// can be checked by hand, including NULLs, result types, result names, and
// a column name that doesn't exist.
//
//      a      b      c
//      5     1.5     7
//    NULL    2.5   NULL
//      3    NULL   NULL
//     -2     4.0   NULL
//
//  a (INT):    5, 3, -2       → count 3, sum 6,   min -2,  max 5,   avg 2.0
//  b (DOUBLE): 1.5, 2.5, 4.0  → count 3, sum 8.0, min 1.5, max 4.0, avg 8/3
//  c (INT):    7              → count 1, sum 7,   min 7,   max 7,   avg 7.0
//
// (The first row has no NULLs because column types are detected from it, so
// an all-NULL column can't be built from a CSV yet.)
//

#include "test_util.h"

#define FILE_BDB "aggregates.bdb"

static void check_int(BdbAggregateType kind, const char *column,
                      int64_t expected, const char *expected_name) {
    AggResult r = run_aggregate(FILE_BDB, kind, column);
    CHECK_STATUS(r.status, BDB_OK, r.err);
    CHECK(r.type == BDB_COL_INT);
    CHECK(r.valid);
    CHECK(r.i == expected);
    CHECK(strcmp(r.name, expected_name) == 0);
    CHECK(r.then_null);                      // the second next() must return NULL
    if (r.i != expected) {
        fprintf(stderr, "  %s: got %" PRId64 ", expected %" PRId64 "\n",
                expected_name, r.i, expected);
    }
}

static void check_double(BdbAggregateType kind, const char *column,
                         double expected, const char *expected_name) {
    AggResult r = run_aggregate(FILE_BDB, kind, column);
    CHECK_STATUS(r.status, BDB_OK, r.err);
    CHECK(r.type == BDB_COL_DOUBLE);
    CHECK(r.valid);
    CHECK(r.d == expected);
    CHECK(strcmp(r.name, expected_name) == 0);
    CHECK(r.then_null);
    if (r.d != expected) {
        fprintf(stderr, "  %s: got %.17g, expected %.17g\n", expected_name, r.d, expected);
    }
}

int main(void) {
    BdbError err = {0};

    REQUIRE(write_text_file("aggregates.csv",
        "a,b,c\n"
        "5,1.5,7\n"
        ",2.5,\n"
        "3,,\n"
        "-2,4.0,\n"));
    CHECK_STATUS(csv_to_bdb("aggregates.csv", FILE_BDB, &err), BDB_OK, err);
    REQUIRE(test_failures == 0);

    // a: INT with one NULL
    check_int   (BDB_AGG_SUM,   "a", 6,   "SUM(a)");
    check_int   (BDB_AGG_COUNT, "a", 3,   "COUNT(a)");
    check_int   (BDB_AGG_MIN,   "a", -2,  "MIN(a)");
    check_int   (BDB_AGG_MAX,   "a", 5,   "MAX(a)");
    check_double(BDB_AGG_AVG,   "a", 2.0, "AVG(a)");        // AVG is always DOUBLE

    // b: DOUBLE with one NULL
    check_double(BDB_AGG_SUM,   "b", 8.0,       "SUM(b)");
    check_int   (BDB_AGG_COUNT, "b", 3,         "COUNT(b)"); // COUNT is always INT
    check_double(BDB_AGG_MIN,   "b", 1.5,       "MIN(b)");
    check_double(BDB_AGG_MAX,   "b", 4.0,       "MAX(b)");
    check_double(BDB_AGG_AVG,   "b", 8.0 / 3.0, "AVG(b)");

    // c: one value, three NULLs
    check_int   (BDB_AGG_SUM,   "c", 7,   "SUM(c)");
    check_int   (BDB_AGG_COUNT, "c", 1,   "COUNT(c)");
    check_int   (BDB_AGG_MIN,   "c", 7,   "MIN(c)");
    check_int   (BDB_AGG_MAX,   "c", 7,   "MAX(c)");
    check_double(BDB_AGG_AVG,   "c", 7.0, "AVG(c)");

    // A column that doesn't exist is an error, not a result.
    AggResult missing = run_aggregate(FILE_BDB, BDB_AGG_SUM, "nope");
    CHECK(missing.status == BDB_ERR_NOT_FOUND);

    // A file that doesn't exist fails when the scan opens it.
    AggResult no_file = run_aggregate("does-not-exist.bdb", BDB_AGG_SUM, "a");
    CHECK(no_file.status == BDB_ERR_OPEN);

    return test_report("test_aggregates");
}
