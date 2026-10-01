//
// test_roundtrip: CSV → .bdb → reader gives back every value, exactly.
//
// Generates a CSV big enough for three row groups (two full, one partial),
// with NULLs in three of the four columns, writes it to .bdb, then streams it
// back and checks every row of every column against the generator. Finally
// runs aggregates over the file and compares them with totals computed while
// generating.
//

#include "test_util.h"

#define ROWS ((uint64_t)2 * BDB_ROW_GROUP_SIZE + 1000)     // 3 row groups

// The generator: what row i holds in each column. id is never NULL; the
// first row has no NULLs, because column types are detected from it.
static bool    qty_null(uint64_t i)   { return i % 7  == 3; }
static bool    price_null(uint64_t i) { return i % 11 == 5; }
static bool    flag_null(uint64_t i)  { return i % 13 == 6; }

static int64_t qty_value(uint64_t i)  { return (int64_t)(i % 90) + 10; }
static bool    flag_value(uint64_t i) { return i % 2 == 0; }

static void price_text(uint64_t i, char *buf, size_t size) {
    snprintf(buf, size, "%.2f", (double)(i % 10000) / 100.0);
}
// The exact double the CSV reader produces from that text.
static double price_value(uint64_t i) {
    char buf[32];
    price_text(i, buf, sizeof buf);
    return strtod(buf, NULL);
}

// Rows the reader should put in the chunk that starts at global row r.
static uint64_t expected_chunk_rows(uint64_t r) {
    uint64_t group       = r / BDB_ROW_GROUP_SIZE;
    uint64_t group_start = group * BDB_ROW_GROUP_SIZE;
    uint64_t group_rows  = ROWS - group_start < BDB_ROW_GROUP_SIZE ? ROWS - group_start : BDB_ROW_GROUP_SIZE;
    uint64_t left        = group_rows - (r - group_start);
    return left < BDB_VECTOR_SIZE ? left : BDB_VECTOR_SIZE;
}

int main(void) {
    BdbError err = {0};

    // --- generate the CSV, and the expected aggregates ---------------------
    uint64_t qty_count = 0,   price_count = 0;
    int64_t  qty_sum   = 0,   qty_min = INT64_MAX, qty_max = INT64_MIN;
    double   price_sum = 0.0;

    FILE *f = fopen("roundtrip.csv", "w");
    REQUIRE(f != NULL);
    fprintf(f, "id,qty,price,flag\n");
    for (uint64_t i = 0; i < ROWS; i++) {
        char price[32] = "";
        if (!price_null(i)) price_text(i, price, sizeof price);

        fprintf(f, "%" PRIu64 ",", i);
        if (!qty_null(i)) fprintf(f, "%" PRId64, qty_value(i));
        fprintf(f, ",%s,%s\n", price, flag_null(i) ? "" : (flag_value(i) ? "true" : "false"));

        if (!qty_null(i)) {
            int64_t q = qty_value(i);
            qty_count++;
            qty_sum += q;
            if (q < qty_min) qty_min = q;
            if (q > qty_max) qty_max = q;
        }
        if (!price_null(i)) {
            price_count++;
            price_sum += price_value(i);      // same order as the aggregate adds them
        }
    }
    REQUIRE(fclose(f) == 0);

    // --- write it -------------------------------------------------------------
    CHECK_STATUS(csv_to_bdb("roundtrip.csv", "roundtrip.bdb", &err), BDB_OK, err);
    REQUIRE(test_failures == 0);

    // --- read it back: the footer -------------------------------------------
    BDB_READER reader = {0};
    CHECK_STATUS(bdb_reader_open(&reader, "roundtrip.bdb", &err), BDB_OK, err);
    REQUIRE(test_failures == 0);

    CHECK(reader.row_count == ROWS);
    CHECK(reader.group_count == 3);
    REQUIRE(reader.col_count == 4);

    const char *names[4]          = { "id", "qty", "price", "flag" };
    const enum ColumnType types[4] = { BDB_COL_INT, BDB_COL_INT, BDB_COL_DOUBLE, BDB_COL_BOOL };
    for (int c = 0; c < 4; c++) {
        CHECK(strcmp(reader.chunk.columns[c].name, names[c]) == 0);
        CHECK(reader.chunk.columns[c].type == types[c]);
    }

    // --- read it back: every value -----------------------------------------
    const CHUNK *chunk = NULL;
    uint64_t row = 0, chunks = 0;
    BdbStatus status;
    while ((status = bdb_reader_next_chunk(&reader, &chunk, &err)) == BDB_OK && chunk != NULL) {
        chunks++;
        CHECK(chunk->count == expected_chunk_rows(row));
        CHECK(chunk->col_count == 4);

        const COLUMN  *cols  = chunk->columns;
        const int64_t *id    = cols[0].data;
        const int64_t *qty   = cols[1].data;
        const double  *price = cols[2].data;
        const bool    *flag  = cols[3].data;

        for (uint64_t k = 0; k < chunk->count; k++) {
            uint64_t i = row + k;

            CHECK(cols[0].bitmap[k] == 1 && id[k] == (int64_t)i);

            CHECK(cols[1].bitmap[k] == (qty_null(i) ? 0 : 1));
            if (!qty_null(i)) CHECK(qty[k] == qty_value(i));

            CHECK(cols[2].bitmap[k] == (price_null(i) ? 0 : 1));
            if (!price_null(i)) CHECK(price[k] == price_value(i));

            CHECK(cols[3].bitmap[k] == (flag_null(i) ? 0 : 1));
            if (!flag_null(i)) CHECK(flag[k] == flag_value(i));
        }
        row += chunk->count;
    }
    CHECK_STATUS(status, BDB_OK, err);
    CHECK(row == ROWS);
    CHECK(chunks == 60 + 60 + 1);          // two full groups of 60 chunks, then 1000 rows

    // After the end, next_chunk keeps returning NULL.
    CHECK_STATUS(bdb_reader_next_chunk(&reader, &chunk, &err), BDB_OK, err);
    CHECK(chunk == NULL);
    bdb_reader_close(&reader);

    // --- aggregates over the whole file --------------------------------------
    AggResult r;

    r = run_aggregate("roundtrip.bdb", BDB_AGG_SUM, "qty");
    CHECK_STATUS(r.status, BDB_OK, r.err);
    CHECK(r.type == BDB_COL_INT && r.valid && r.i == qty_sum);

    r = run_aggregate("roundtrip.bdb", BDB_AGG_COUNT, "qty");
    CHECK_STATUS(r.status, BDB_OK, r.err);
    CHECK(r.type == BDB_COL_INT && r.i == (int64_t)qty_count);

    r = run_aggregate("roundtrip.bdb", BDB_AGG_MIN, "qty");
    CHECK_STATUS(r.status, BDB_OK, r.err);
    CHECK(r.i == qty_min);

    r = run_aggregate("roundtrip.bdb", BDB_AGG_MAX, "qty");
    CHECK_STATUS(r.status, BDB_OK, r.err);
    CHECK(r.i == qty_max);

    r = run_aggregate("roundtrip.bdb", BDB_AGG_SUM, "price");
    CHECK_STATUS(r.status, BDB_OK, r.err);
    CHECK(r.type == BDB_COL_DOUBLE && r.d == price_sum);     // exact: same values, same order

    r = run_aggregate("roundtrip.bdb", BDB_AGG_AVG, "price");
    CHECK_STATUS(r.status, BDB_OK, r.err);
    CHECK(r.type == BDB_COL_DOUBLE && r.d == price_sum / (double)price_count);

    r = run_aggregate("roundtrip.bdb", BDB_AGG_COUNT, "id");
    CHECK_STATUS(r.status, BDB_OK, r.err);
    CHECK(r.i == (int64_t)ROWS);

    return test_report("test_roundtrip");
}
