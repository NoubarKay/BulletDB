//
// Created by user on 9/24/2026.
//
#include <inttypes.h>
#include <stdio.h>

#include "common/common.h"
#include "csv/csv_reader.h"
#include "executor/explain.h"
#include "logical/bdb_expr.h"
#include "logical/bdb_logical_plan.h"
#include "planner/bdb_planner.h"
#include "storage/bdb_writer.h"

#define BDB_PATH "test-1.bdb"

// Imports a CSV file into a .bdb file, one chunk at a time.
static BdbStatus import_csv(const char *csv_path, const char *bdb_path, BdbError *err) {
    CSV_READER reader = {0};
    BDB_WRITER writer = {0};
    const CHUNK *chunk;

    BdbStatus status = csv_open(&reader, csv_path, err);
    if (status != BDB_OK) return status;

    status = bdb_writer_open(&writer, bdb_path, err);
    while (status == BDB_OK &&
           (status = csv_next_chunk(&reader, &chunk, err)) == BDB_OK && chunk != NULL) {
        status = bdb_writer_append(&writer, chunk, err);
    }
    if (status == BDB_OK) status = bdb_writer_finish(&writer, err);

    bdb_writer_close(&writer);
    csv_close(&reader);
    return status;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file.csv>\n", argv[0]);
        return 1;
    }

    BdbError err = {0};
    BdbStatus status = import_csv(argv[1], BDB_PATH, &err);
    if (status != BDB_OK) {
        fprintf(stderr, "error: %s\n", err.message);
        return 1;
    }

    // The query, as a logical plan:
    //   SELECT AVG(QUANTITYORDERED) WHERE QUANTITYORDERED >= 30
    // The schema is written by hand for now and must match the CSV's columns.
    BdbField sales_fields[] = {
        { "ORDERNUMBER",     BDB_COL_INT },
        { "QUANTITYORDERED", BDB_COL_INT },
        { "PRICEEACH",       BDB_COL_DOUBLE },
        { "ORDERLINENUMBER", BDB_COL_INT },
        { "SALES",           BDB_COL_DOUBLE },
    };
    BdbSchema sales = { sales_fields, 5 };

    BdbExpr qty    = bdb_expr_column("QUANTITYORDERED");
    BdbExpr thirty = bdb_expr_literal_long(30);
    BdbExpr cond   = bdb_expr_binary(BDB_BINARY_EXPR_GE, &qty, &thirty);

    BdbLogicalPlan scan = bdb_plan_scan(BDB_PATH, sales);
    BdbLogicalPlan filter;
    status = bdb_plan_selection(&filter, &scan, &cond, &err);
    if (status != BDB_OK) {
        fprintf(stderr, "error: %s\n", err.message);
        return 1;
    }

    BdbExpr avg = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_AVG, &qty);
    const BdbExpr *exprs[] = { &avg };
    BdbField agg_fields[1];
    BdbLogicalPlan agg;
    status = bdb_plan_aggregate(&agg, &filter, exprs, 1, agg_fields, &err);
    if (status != BDB_OK) {
        fprintf(stderr, "error: %s\n", err.message);
        return 1;
    }
    bdb_plan_print(&agg, stdout);
    printf("\n");

    // Turn it into operators and run it.
    BdbOperator *op = NULL;
    status = bdb_planner_create(&agg, &op, &err);
    if (status != BDB_OK) {
        fprintf(stderr, "error: %s\n", err.message);
        return 1;
    }

    // The aggregate returns one chunk with one row, then NULL. AVG is always a
    // DOUBLE; it's NULL (bitmap 0) when no row passed the filter.
    const CHUNK *chunk;
    while ((status = bdb_op_next(op, &chunk, &err)) == BDB_OK && chunk != NULL) {
        const COLUMN *result = &chunk->columns[0];
        if (result->bitmap[0]) {
            printf("%s = %.2f\n\n", result->name, ((const double *)result->data)[0]);
        } else {
            printf("%s = NULL\n\n", result->name);
        }
    }
    if (status != BDB_OK) {
        fprintf(stderr, "error: %s\n", err.message);
    } else {
        bdb_explain(op, stdout);
    }

    bdb_planner_free(op);
    return status == BDB_OK ? 0 : 1;
}
