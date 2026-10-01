//
// test_filter: the FILTER operator (WHERE column op value) with selection
// vectors.
//
// Part 1 uses a small file whose answers can be checked by hand:
//
//     id  qty   price  flag
//      1   10    1.5   true
//      2   20    2.5   false
//      3  NULL   3.5   true
//      4   40   NULL   false
//      5   50    5.5   true
//      6   60    6.0   false
//
// It checks all six comparisons, that NULL never matches, that complementary
// filters add up, that aggregates only see the selected rows, two stacked
// filters, a filter that matches nothing, and the error cases.
//
// Part 2 uses a generated file of two row groups (127,880 rows), to check
// filters across many chunks, including runs of chunks with no match at all.
//
// Not tested yet (known gaps, see docs/KNOWN_ISSUES.md): a fractional
// constant on an INT column (qty = 50.5 currently compares as qty = 50), and
// integer constants above 2^53, which lose precision when parsed as a double.
//

#include "test_util.h"
#include "executor/filter.h"

#define SMALL_BDB "filter_small.bdb"
#define BIG_BDB   "filter_big.bdb"

// One WHERE condition.
typedef struct {
    const char  *column;
    BdbCompareOp op;
    const char  *value;
} Cond;

// Runs  AGGREGATE kind(column) ← FILTER conds[n-1] ← ... ← FILTER conds[0] ← SCAN
// (each condition is its own FILTER operator, so they combine with AND).
static AggResult run_filtered(const char *bdb_path, const Cond *conds, int n,
                              BdbAggregateType kind, const char *column) {
    AggResult res = {0};
    BdbScan scan;
    BdbFilter filters[4];
    BdbAggregate agg;

    res.status = bdb_scan_open(&scan, bdb_path, &res.err);
    BdbOperator *top = &scan.base;
    for (int i = 0; i < n && i < 4; i++) {
        bdb_filter_init(&filters[i], top, conds[i].column, conds[i].op, conds[i].value);
        top = &filters[i].base;
    }
    bdb_aggregate_init(&agg, top, kind, column);
    BdbOperator *op = &agg.base;

    if (res.status == BDB_OK) collect_aggregate(op, &res);

    op->close(op);      // closes the aggregate, every filter, and the scan
    return res;
}

// COUNT(count_column) with one condition. Returns -1 (and records a failure)
// if the query itself fails.
static int64_t count_where(const char *bdb_path, const char *count_column,
                           const char *column, BdbCompareOp op, const char *value) {
    Cond c = { column, op, value };
    AggResult r = run_filtered(bdb_path, &c, 1, BDB_AGG_COUNT, count_column);
    CHECK_STATUS(r.status, BDB_OK, r.err);
    return r.status == BDB_OK ? r.i : -1;
}

#define CHECK_COUNT(bdb, count_col, col, op, value, expected)                    \
    do {                                                                         \
        int64_t got_ = count_where((bdb), (count_col), (col), (op), (value));    \
        if (got_ != (int64_t)(expected)) {                                       \
            fprintf(stderr, "%s:%d: COUNT(%s) WHERE %s %s %s: got %" PRId64      \
                    ", expected %" PRId64 "\n", __FILE__, __LINE__, (count_col), \
                    (col), #op, (value), got_, (int64_t)(expected));             \
            test_failures++;                                                     \
        }                                                                        \
    } while (0)

// ---------------------------------------------------------------------------
// Part 1: the small file
// ---------------------------------------------------------------------------

static void test_small(void) {
    BdbError err = {0};
    if (!write_text_file("filter_small.csv",
        "id,qty,price,flag\n"
        "1,10,1.5,true\n"
        "2,20,2.5,false\n"
        "3,,3.5,true\n"
        "4,40,,false\n"
        "5,50,5.5,true\n"
        "6,60,6.0,false\n")) {
        fprintf(stderr, "could not write filter_small.csv\n");
        test_failures++;
        return;
    }
    CHECK_STATUS(csv_to_bdb("filter_small.csv", SMALL_BDB, &err), BDB_OK, err);

    // All six comparisons on an INT column. Row 3's qty is NULL and must
    // never match, not even for != (so = and != together give 5, not 6).
    CHECK_COUNT(SMALL_BDB, "id", "qty", BDB_COMPARE_EQ, "40", 1);
    CHECK_COUNT(SMALL_BDB, "id", "qty", BDB_COMPARE_NE, "40", 4);
    CHECK_COUNT(SMALL_BDB, "id", "qty", BDB_COMPARE_LT, "40", 2);
    CHECK_COUNT(SMALL_BDB, "id", "qty", BDB_COMPARE_LE, "40", 3);
    CHECK_COUNT(SMALL_BDB, "id", "qty", BDB_COMPARE_GT, "40", 2);
    CHECK_COUNT(SMALL_BDB, "id", "qty", BDB_COMPARE_GE, "40", 3);

    // The same on a DOUBLE column (row 4's price is NULL).
    CHECK_COUNT(SMALL_BDB, "id", "price", BDB_COMPARE_EQ, "6",   1);   // "6" matches 6.0
    CHECK_COUNT(SMALL_BDB, "id", "price", BDB_COMPARE_NE, "2.5", 4);
    CHECK_COUNT(SMALL_BDB, "id", "price", BDB_COMPARE_LT, "3.5", 2);
    CHECK_COUNT(SMALL_BDB, "id", "price", BDB_COMPARE_LE, "3.5", 3);
    CHECK_COUNT(SMALL_BDB, "id", "price", BDB_COMPARE_GT, "2.5", 3);
    CHECK_COUNT(SMALL_BDB, "id", "price", BDB_COMPARE_GE, "2.5", 4);

    // Aggregates only see the selected rows.
    {
        Cond c = { "qty", BDB_COMPARE_GE, "40" };              // rows 4, 5, 6
        AggResult r = run_filtered(SMALL_BDB, &c, 1, BDB_AGG_SUM, "price");
        CHECK_STATUS(r.status, BDB_OK, r.err);
        CHECK(r.valid && r.d == 5.5 + 6.0);                    // row 4's price is NULL

        r = run_filtered(SMALL_BDB, &c, 1, BDB_AGG_COUNT, "price");
        CHECK(r.i == 2);

        r = run_filtered(SMALL_BDB, &c, 1, BDB_AGG_SUM, "qty");
        CHECK(r.i == 40 + 50 + 60);
    }
    {
        Cond c = { "qty", BDB_COMPARE_GT, "15" };
        AggResult r = run_filtered(SMALL_BDB, &c, 1, BDB_AGG_MIN, "qty");
        CHECK_STATUS(r.status, BDB_OK, r.err);
        CHECK(r.i == 20);                                      // every result satisfies the filter
    }
    {
        Cond c = { "qty", BDB_COMPARE_LT, "50" };
        AggResult r = run_filtered(SMALL_BDB, &c, 1, BDB_AGG_MAX, "qty");
        CHECK_STATUS(r.status, BDB_OK, r.err);
        CHECK(r.i == 40);
    }

    // Two stacked filters (AND): qty >= 20 AND price < 6 → rows 2 and 5.
    // The second filter receives an already-selected chunk, so this checks
    // that it follows the input's selection and stores physical rows.
    {
        Cond both[2] = { { "qty", BDB_COMPARE_GE, "20" }, { "price", BDB_COMPARE_LT, "6" } };
        AggResult r = run_filtered(SMALL_BDB, both, 2, BDB_AGG_COUNT, "id");
        CHECK_STATUS(r.status, BDB_OK, r.err);
        CHECK(r.i == 2);

        r = run_filtered(SMALL_BDB, both, 2, BDB_AGG_SUM, "qty");
        CHECK(r.i == 20 + 50);

        // The same two conditions in the other order give the same rows.
        Cond swapped[2] = { both[1], both[0] };
        r = run_filtered(SMALL_BDB, swapped, 2, BDB_AGG_SUM, "id");
        CHECK(r.i == 2 + 5);
    }

    // A filter that matches nothing: COUNT is 0, SUM is NULL.
    {
        Cond c = { "qty", BDB_COMPARE_GT, "1000" };
        AggResult r = run_filtered(SMALL_BDB, &c, 1, BDB_AGG_COUNT, "id");
        CHECK_STATUS(r.status, BDB_OK, r.err);
        CHECK(r.valid && r.i == 0);

        r = run_filtered(SMALL_BDB, &c, 1, BDB_AGG_SUM, "qty");
        CHECK_STATUS(r.status, BDB_OK, r.err);
        CHECK(!r.valid);
    }

    // Errors: a column that doesn't exist, and a type that can't be filtered.
    {
        Cond c = { "nope", BDB_COMPARE_EQ, "1" };
        AggResult r = run_filtered(SMALL_BDB, &c, 1, BDB_AGG_COUNT, "id");
        CHECK(r.status == BDB_ERR_NOT_FOUND);
    }
    {
        Cond c = { "flag", BDB_COMPARE_EQ, "1" };
        AggResult r = run_filtered(SMALL_BDB, &c, 1, BDB_AGG_COUNT, "id");
        CHECK(r.status == BDB_ERR_INVALID);
    }
}

// ---------------------------------------------------------------------------
// Part 2: many chunks, two row groups
// ---------------------------------------------------------------------------

#define BIG_ROWS ((uint64_t)BDB_ROW_GROUP_SIZE + 5000)     // 2 row groups

static void test_big(void) {
    BdbError err = {0};

    // w = row number, v = w % 100
    FILE *f = fopen("filter_big.csv", "w");
    if (f == NULL) {
        fprintf(stderr, "could not write filter_big.csv\n");
        test_failures++;
        return;
    }
    fprintf(f, "w,v\n");
    for (uint64_t i = 0; i < BIG_ROWS; i++) {
        fprintf(f, "%" PRIu64 ",%" PRIu64 "\n", i, i % 100);
    }
    fclose(f);
    CHECK_STATUS(csv_to_bdb("filter_big.csv", BIG_BDB, &err), BDB_OK, err);

    // How many rows have v < 10, and v == 7, counted directly.
    uint64_t below_10 = 0, equal_7 = 0;
    for (uint64_t i = 0; i < BIG_ROWS; i++) {
        if (i % 100 < 10)  below_10++;
        if (i % 100 == 7)  equal_7++;
    }

    // A match in every chunk.
    CHECK_COUNT(BIG_BDB, "w", "v", BDB_COMPARE_LT, "10", below_10);

    // Complementary filters cover every row exactly once.
    CHECK_COUNT(BIG_BDB, "w", "v", BDB_COMPARE_EQ, "7", equal_7);
    CHECK_COUNT(BIG_BDB, "w", "v", BDB_COMPARE_NE, "7", BIG_ROWS - equal_7);

    // Matches only in the last chunk: every chunk before it has no match.
    char last10[32];
    snprintf(last10, sizeof last10, "%" PRIu64, BIG_ROWS - 10);
    CHECK_COUNT(BIG_BDB, "w", "w", BDB_COMPARE_GE, last10, 10);
    {
        Cond c = { "w", BDB_COMPARE_GE, last10 };
        AggResult r = run_filtered(BIG_BDB, &c, 1, BDB_AGG_SUM, "w");
        CHECK_STATUS(r.status, BDB_OK, r.err);
        int64_t expected = 0;
        for (uint64_t i = BIG_ROWS - 10; i < BIG_ROWS; i++) expected += (int64_t)i;
        CHECK(r.i == expected);
    }

    // Matches only in the first chunk: every chunk after it has no match.
    CHECK_COUNT(BIG_BDB, "w", "w", BDB_COMPARE_LT, "5", 5);

    // Matches across the boundary between the two row groups.
    char lo[32], hi[32];
    snprintf(lo, sizeof lo, "%d", BDB_ROW_GROUP_SIZE - 3);
    snprintf(hi, sizeof hi, "%d", BDB_ROW_GROUP_SIZE + 3);
    {
        Cond around[2] = { { "w", BDB_COMPARE_GE, lo }, { "w", BDB_COMPARE_LT, hi } };
        AggResult r = run_filtered(BIG_BDB, around, 2, BDB_AGG_COUNT, "w");
        CHECK_STATUS(r.status, BDB_OK, r.err);
        CHECK(r.i == 6);
    }
}

int main(void) {
    test_small();
    test_big();
    return test_report("test_filter");
}
