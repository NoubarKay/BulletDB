//
// test_stats: the per-row-group min and max in the footer (format v2).
//
// Generates a CSV that fills one row group and part of a second, writes it to
// .bdb, and checks every group's stats against values computed from the
// generator: NULLs left out, negative numbers, a BOOL column, and a column
// that is entirely NULL in the first group. Then checks that the stats,
// combined over the groups, equal MIN and MAX from a scan. A second, tiny
// file covers the edge cases with known answers.
//

#include "test_util.h"

#define ROWS ((uint64_t)BDB_ROW_GROUP_SIZE + 500)       // 2 row groups
#define COLS 5

// The generator: what row i holds in each column. id is never NULL, and
// late is NULL in every row of the first group.
static bool    qty_null(uint64_t i)   { return i % 7  == 3; }
static bool    price_null(uint64_t i) { return i % 11 == 5; }
static bool    flag_null(uint64_t i)  { return i % 13 == 6; }
static bool    late_null(uint64_t i)  { return i < BDB_ROW_GROUP_SIZE; }

static int64_t qty_value(uint64_t i)  { return (int64_t)(i % 90) - 40; }       // -40 to 49
static bool    flag_value(uint64_t i) { return i % 2 == 0; }
static int64_t late_value(uint64_t i) { return (int64_t)(i % 50) + 100; }      // 100 to 149

static void price_text(uint64_t i, char *buf, size_t size) {
    snprintf(buf, size, "%.2f", (double)(i % 10000) / 100.0 - 25.0);          // -25.00 to 74.99
}
// The exact double the CSV reader produces from that text.
static double price_value(uint64_t i) {
    char buf[32];
    price_text(i, buf, sizeof buf);
    return strtod(buf, NULL);
}

// Folds one value into a stats entry, the way the writer should.
static void add_int(BdbColumnStats *s, int64_t v) {
    if (!s->has_minmax) {
        s->min.i = s->max.i = v;
        s->has_minmax = true;
        return;
    }
    if (v < s->min.i) s->min.i = v;
    if (v > s->max.i) s->max.i = v;
}

static void add_double(BdbColumnStats *s, double v) {
    if (!s->has_minmax) {
        s->min.d = s->max.d = v;
        s->has_minmax = true;
        return;
    }
    if (v < s->min.d) s->min.d = v;
    if (v > s->max.d) s->max.d = v;
}

// Compares one stats entry from the footer with the expected one.
static void check_stats(const char *what, const BdbColumnStats *got, const BdbColumnStats *want,
                        enum ColumnType type) {
    bool same = got->has_minmax == want->has_minmax;
    if (type == BDB_COL_DOUBLE) {
        same = same && got->min.d == want->min.d && got->max.d == want->max.d;
    } else {
        same = same && got->min.i == want->min.i && got->max.i == want->max.i;
    }
    if (same) return;

    if (type == BDB_COL_DOUBLE) {
        fprintf(stderr, "%s: got has_minmax %d, min %.17g, max %.17g; expected %d, %.17g, %.17g\n",
                what, got->has_minmax, got->min.d, got->max.d,
                want->has_minmax, want->min.d, want->max.d);
    } else {
        fprintf(stderr, "%s: got has_minmax %d, min %" PRId64 ", max %" PRId64
                        "; expected %d, %" PRId64 ", %" PRId64 "\n",
                what, got->has_minmax, got->min.i, got->max.i,
                want->has_minmax, want->min.i, want->max.i);
    }
    test_failures++;
}

// A column with no values must be stored as has_minmax = 0 and all-zero
// min and max, so the same input always gives the same file.
static void check_no_values(const char *what, const BdbColumnStats *s) {
    if (!s->has_minmax && s->min.i == 0 && s->max.i == 0) return;
    fprintf(stderr, "%s: expected has_minmax 0 and zeroed min/max, got %d, 0x%016" PRIX64
                    ", 0x%016" PRIX64 "\n",
            what, s->has_minmax, (uint64_t)s->min.i, (uint64_t)s->max.i);
    test_failures++;
}

// Two row groups: every group's stats, then the whole file against a scan.
static int test_generated_file(void) {
    BdbError err = {0};

    FILE *f = fopen("stats.csv", "w");
    REQUIRE(f != NULL);
    fprintf(f, "id,qty,price,flag,late\n");
    for (uint64_t i = 0; i < ROWS; i++) {
        char price[32] = "";
        if (!price_null(i)) price_text(i, price, sizeof price);

        fprintf(f, "%" PRIu64 ",", i);
        if (!qty_null(i)) fprintf(f, "%" PRId64, qty_value(i));
        fprintf(f, ",%s,%s,", price, flag_null(i) ? "" : (flag_value(i) ? "true" : "false"));
        if (!late_null(i)) fprintf(f, "%" PRId64, late_value(i));
        fprintf(f, "\n");
    }
    REQUIRE(fclose(f) == 0);

    CHECK_STATUS(csv_to_bdb("stats.csv", "stats.bdb", &err), BDB_OK, err);
    REQUIRE(test_failures == 0);

    BDB_READER reader = {0};
    CHECK_STATUS(bdb_reader_open(&reader, "stats.bdb", &err), BDB_OK, err);
    REQUIRE(test_failures == 0);

    REQUIRE(reader.row_count == ROWS);
    REQUIRE(reader.group_count == 2);
    REQUIRE(reader.col_count == COLS);
    REQUIRE(reader.stats != NULL);

    // late is empty in every sampled row, so the sniffer makes it DOUBLE.
    const char *names[COLS] = { "id", "qty", "price", "flag", "late" };
    const enum ColumnType types[COLS] = {
        BDB_COL_INT, BDB_COL_INT, BDB_COL_DOUBLE, BDB_COL_BOOL, BDB_COL_DOUBLE
    };
    for (int c = 0; c < COLS; c++) {
        CHECK(reader.chunk.columns[c].type == types[c]);
    }
    REQUIRE(test_failures == 0);

    // --- each group against the generator ------------------------------------
    BdbColumnStats file_stats[COLS] = {0};      // the footer's stats, combined over the groups

    for (uint32_t g = 0; g < reader.group_count; g++) {
        uint64_t start = (uint64_t)g * BDB_ROW_GROUP_SIZE;
        uint64_t end   = start + reader.row_groups[g];
        CHECK(end <= ROWS);

        BdbColumnStats want[COLS] = {0};
        for (uint64_t i = start; i < end && i < ROWS; i++) {
            add_int(&want[0], (int64_t)i);
            if (!qty_null(i))   add_int(&want[1], qty_value(i));
            if (!price_null(i)) add_double(&want[2], price_value(i));
            if (!flag_null(i))  add_int(&want[3], flag_value(i));
            if (!late_null(i))  add_double(&want[4], (double)late_value(i));
        }

        for (int c = 0; c < COLS; c++) {
            const BdbColumnStats *got = &reader.stats[(size_t)g * COLS + c];
            char what[64];
            snprintf(what, sizeof what, "group %u, %s", g, names[c]);
            check_stats(what, got, &want[c], types[c]);

            if (!got->has_minmax) continue;
            if (types[c] == BDB_COL_DOUBLE) {
                add_double(&file_stats[c], got->min.d);
                add_double(&file_stats[c], got->max.d);
            } else {
                add_int(&file_stats[c], got->min.i);
                add_int(&file_stats[c], got->max.i);
            }
        }
    }

    // Spot checks with the answers written out, so a mistake shared by the
    // writer and the helpers above can't hide.
    const BdbColumnStats *group0 = &reader.stats[0];
    const BdbColumnStats *group1 = &reader.stats[COLS];
    CHECK(group0[0].min.i == 0 && group0[0].max.i == BDB_ROW_GROUP_SIZE - 1);
    CHECK(group1[0].min.i == BDB_ROW_GROUP_SIZE && group1[0].max.i == (int64_t)ROWS - 1);
    CHECK(group0[1].min.i == -40 && group0[1].max.i == 49);
    CHECK(group0[2].min.d == -25.0 && group0[2].max.d == strtod("74.99", NULL));
    CHECK(group0[3].has_minmax && group0[3].min.i == 0 && group0[3].max.i == 1);
    check_no_values("group 0, late (all NULL)", &group0[4]);
    CHECK(group1[4].has_minmax && group1[4].min.d == 100.0 && group1[4].max.d == 149.0);

    bdb_reader_close(&reader);

    // --- the whole file against a scan ---------------------------------------
    // The smallest group min is the file's MIN, and the largest group max is
    // its MAX. (BOOL is left out: aggregates don't accept it.)
    for (int c = 0; c < COLS; c++) {
        if (types[c] == BDB_COL_BOOL) continue;
        REQUIRE(file_stats[c].has_minmax);

        AggResult low  = run_aggregate("stats.bdb", BDB_AGG_MIN, names[c]);
        AggResult high = run_aggregate("stats.bdb", BDB_AGG_MAX, names[c]);
        CHECK_STATUS(low.status, BDB_OK, low.err);
        CHECK_STATUS(high.status, BDB_OK, high.err);
        if (types[c] == BDB_COL_DOUBLE) {
            CHECK(low.valid && low.d == file_stats[c].min.d);
            CHECK(high.valid && high.d == file_stats[c].max.d);
        } else {
            CHECK(low.valid && low.i == file_stats[c].min.i);
            CHECK(high.valid && high.i == file_stats[c].max.i);
        }
    }
    return 0;
}

// Two rows, with the answers known in advance.
static int test_edge_cases(void) {
    BdbError err = {0};

    //   yes    BOOL, always true            -> min 1, max 1
    //   no     BOOL, always false           -> min 0, max 0, but has_minmax 1
    //   empty  every row NULL (DOUBLE)      -> has_minmax 0
    //   late   NULL, then 7                 -> min 7, max 7: the NULL's stored 0 doesn't count
    //   x      5, then -3                   -> min -3, max 5: the first row isn't always the min
    REQUIRE(write_text_file("stats_small.csv",
        "yes,no,empty,late,x\n"
        "true,false,,,5\n"
        "true,false,,7,-3\n"));
    CHECK_STATUS(csv_to_bdb("stats_small.csv", "stats_small.bdb", &err), BDB_OK, err);
    REQUIRE(test_failures == 0);

    BDB_READER reader = {0};
    CHECK_STATUS(bdb_reader_open(&reader, "stats_small.bdb", &err), BDB_OK, err);
    REQUIRE(test_failures == 0);
    REQUIRE(reader.group_count == 1 && reader.col_count == 5 && reader.stats != NULL);

    const COLUMN *cols = reader.chunk.columns;
    REQUIRE(cols[0].type == BDB_COL_BOOL && cols[1].type == BDB_COL_BOOL &&
            cols[3].type == BDB_COL_INT  && cols[4].type == BDB_COL_INT);

    const BdbColumnStats *s = reader.stats;
    CHECK(s[0].has_minmax && s[0].min.i == 1 && s[0].max.i == 1);
    CHECK(s[1].has_minmax && s[1].min.i == 0 && s[1].max.i == 0);
    check_no_values("empty (all NULL)", &s[2]);
    CHECK(s[3].has_minmax && s[3].min.i == 7 && s[3].max.i == 7);
    CHECK(s[4].has_minmax && s[4].min.i == -3 && s[4].max.i == 5);

    bdb_reader_close(&reader);
    return 0;
}

int main(void) {
    if (test_generated_file() != 0) return 1;
    if (test_edge_cases() != 0) return 1;
    return test_report("test_stats");
}
