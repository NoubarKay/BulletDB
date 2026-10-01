//
// Shared helpers for the BulletDB tests: check macros, file helpers, the
// CSV → .bdb import, and running one aggregate query.
//

#ifndef BULLETDB_TEST_UTIL_H
#define BULLETDB_TEST_UTIL_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/common.h"
#include "csv/csv_reader.h"
#include "executor/aggregate.h"
#include "executor/scan.h"
#include "storage/bdb_reader.h"
#include "storage/bdb_writer.h"

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------

static int test_failures = 0;

// Records a failure and keeps going. Only the first 25 failures are printed,
// so a broken loop over thousands of rows doesn't flood the log.
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            if (test_failures < 25)                                              \
                fprintf(stderr, "%s:%d: CHECK failed: %s\n",                     \
                        __FILE__, __LINE__, #cond);                              \
            test_failures++;                                                     \
        }                                                                        \
    } while (0)

// Like CHECK, but for a BdbStatus. Prints the error message on failure.
#define CHECK_STATUS(expr, expected, err)                                        \
    do {                                                                         \
        BdbStatus s_ = (expr);                                                   \
        if (s_ != (expected)) {                                                  \
            fprintf(stderr, "%s:%d: %s returned %d (%s: %s), expected %d\n",     \
                    __FILE__, __LINE__, #expr, (int)s_, bdb_status_str(s_),      \
                    (err).message, (int)(expected));                             \
            test_failures++;                                                     \
        }                                                                        \
    } while (0)

// Stops the test: for setup steps that everything after them depends on.
#define REQUIRE(cond)                                                            \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: REQUIRE failed: %s\n",                       \
                    __FILE__, __LINE__, #cond);                                  \
            return 1;                                                            \
        }                                                                        \
    } while (0)

// Prints the summary; returns the process exit code.
static inline int test_report(const char *name) {
    if (test_failures == 0) {
        printf("%s: all checks passed\n", name);
        return 0;
    }
    fprintf(stderr, "%s: %d check(s) failed\n", name, test_failures);
    return 1;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

static inline bool write_text_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f == NULL) return false;
    bool ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

static inline bool write_bytes(const char *path, const unsigned char *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) return false;
    bool ok = size == 0 || fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

// Reads a whole file into a malloc'd buffer. The caller frees it.
static inline unsigned char *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    unsigned char *data = NULL;
    size_t used = 0, cap = 0;
    for (;;) {
        if (used == cap) {
            cap = cap ? cap * 2 : 4096;
            unsigned char *grown = realloc(data, cap);
            if (grown == NULL) { free(data); fclose(f); return NULL; }
            data = grown;
        }
        size_t got = fread(data + used, 1, cap - used, f);
        used += got;
        if (got == 0) break;
    }
    fclose(f);
    *size = used;
    return data;
}

// ---------------------------------------------------------------------------
// Engine helpers
// ---------------------------------------------------------------------------

// CSV → .bdb, the same steps as main.c.
static inline BdbStatus csv_to_bdb(const char *csv_path, const char *bdb_path, BdbError *err) {
    CSV_READER reader = {0};
    BDB_WRITER writer = {0};
    const CHUNK *chunk = NULL;

    BdbStatus status = csv_open(&reader, csv_path, err);
    if (status == BDB_OK) status = bdb_writer_open(&writer, bdb_path, err);

    while (status == BDB_OK &&
           (status = csv_next_chunk(&reader, &chunk, err)) == BDB_OK && chunk != NULL) {
        status = bdb_writer_append(&writer, chunk, err);
    }
    if (status == BDB_OK) status = bdb_writer_finish(&writer, err);

    bdb_writer_close(&writer);
    csv_close(&reader);
    return status;
}

// The result of one aggregate query, copied out of the result chunk.
typedef struct {
    BdbStatus       status;
    BdbError        err;
    char            name[64];          // e.g. "SUM(qty)"
    enum ColumnType type;              // result column type
    bool            valid;             // false = the result is NULL
    int64_t         i;                 // value, if type is INT
    double          d;                 // value, if type is DOUBLE
    bool            then_null;         // did the second next() return NULL?
} AggResult;

// Pulls the result out of an aggregate operator that sits on top of any tree,
// copies it into `res`, then checks that a second next() returns NULL.
// Leaves `res->status` as an error if any step fails. Doesn't close `op`.
static inline void collect_aggregate(BdbOperator *op, AggResult *res) {
    const CHUNK *chunk = NULL;
    res->status = bdb_op_next(op, &chunk, &res->err);
    if (res->status != BDB_OK) return;

    if (chunk == NULL || chunk->count != 1 || chunk->col_count != 1) {
        res->status = BDB_ERR_INVALID;          // the result must be 1 column × 1 row
        return;
    }
    const COLUMN *col = &chunk->columns[0];
    snprintf(res->name, sizeof res->name, "%s", col->name ? col->name : "");
    res->type  = col->type;
    res->valid = col->bitmap[0] != 0;
    if (col->type == BDB_COL_INT)    res->i = ((const int64_t *)col->data)[0];
    if (col->type == BDB_COL_DOUBLE) res->d = ((const double *)col->data)[0];

    const CHUNK *again = NULL;
    BdbStatus second = bdb_op_next(op, &again, &res->err);
    res->then_null = second == BDB_OK && again == NULL;
}

// Runs  AGGREGATE kind(column) ← SCAN bdb_path  and copies out the result.
static inline AggResult run_aggregate(const char *bdb_path, BdbAggregateType kind, const char *column) {
    AggResult res = {0};
    BdbScan scan;
    BdbAggregate agg;

    res.status = bdb_scan_open(&scan, bdb_path, &res.err);
    bdb_aggregate_init(&agg, &scan.base, kind, column);
    BdbOperator *op = &agg.base;

    if (res.status == BDB_OK) collect_aggregate(op, &res);

    op->close(op);     // closes the aggregate and the scan
    return res;
}

#endif //BULLETDB_TEST_UTIL_H
