//
// Created by user on 9/24/2026.
//
#define _CRTDBG_MAP_ALLOC
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/table.h"
#include "csv/csv_reader.h"
#include "common/common.h"
#include "executor/aggregate.h"
#include "executor/scan.h"
#include "executor/explain.h"
#include "executor/filter.h"
#include "logical/bdb_expr.h"
#include "storage/bdb_format.h"
#include "storage/bdb_reader.h"
#include "storage/bdb_writer.h"
#include "storage/debug.h"

// Prints one chunk as a table.
static BdbStatus print_chunk_cb(const CHUNK *chunk, BdbError *err) {
    (void)err;


    // A chunk has the same shape as a table, so borrow print_table.
    TABLE view = {
        .row_count = chunk->count,
        .col_count = chunk->col_count,
        .columns = chunk->columns,
        .sel_vector = chunk->sel_vector,
    };
    print_table(&view);
    return BDB_OK;
}

// Prints one logical expression, then what bdb_expr_to_field says it produces
// against `schema`: a field's name and type, or the error.
static void show_expr(const char *label, const BdbExpr *expr, const BdbSchema *schema) {
    BdbField field;
    BdbError err = {0};

    printf("%-28s prints as  ", label);
    bdb_expr_print(expr, stdout);

    if (bdb_expr_to_field(expr, schema, &field, &err) != BDB_OK) {
        printf("\n%-28s error: %s\n\n", "", err.message);
        return;
    }
    const char *type = field.type == BDB_COL_INT    ? "INT"
                     : field.type == BDB_COL_DOUBLE ? "DOUBLE"
                     : field.type == BDB_COL_BOOL   ? "BOOL" : "STR";
    printf("\n%-28s field: name \"%s\", type %s\n\n", "", field.name, type);
}

// A tour of the logical expressions, checked against a made-up employees table.
static void show_logical_expressions(void) {
    BdbField fields[] = {
        { "name",       BDB_COL_STR },
        { "salary",     BDB_COL_DOUBLE },
        { "department", BDB_COL_STR },
        { "age",        BDB_COL_INT },
    };
    BdbSchema employees = { fields, 4 };

    BdbExpr salary      = bdb_expr_column("salary");
    BdbExpr age         = bdb_expr_column("age");
    BdbExpr missing     = bdb_expr_column("height");            // not in the table
    BdbExpr forty_five  = bdb_expr_literal_long(45);
    BdbExpr negative    = bdb_expr_literal_long(-7);
    BdbExpr factor      = bdb_expr_literal_double(1.1);
    BdbExpr engineering = bdb_expr_literal_string("Engineering");

    printf("== logical expressions ==\n\n");
    show_expr("column salary",                &salary,      &employees);
    show_expr("column age",                   &age,         &employees);
    show_expr("column height (missing)",      &missing,     &employees);
    show_expr("literal long 45",              &forty_five,  &employees);
    show_expr("literal long -7",              &negative,    &employees);
    show_expr("literal double 1.1",           &factor,      &employees);
    show_expr("literal string Engineering",   &engineering, &employees);
}

int main(int argc, char *argv[]) {
    show_logical_expressions();

    BdbError err = {0};
    CSV_READER csvReader = {0};
    BDB_WRITER writer = {0};
    BdbScan scan; BdbFilter filter; BdbFilter filter2; BdbAggregate agg;

    const CHUNK *chunk;
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "Usage: %s <file.csv> [column to SUM]\n", argv[0]);
        return 1;
    }


    BdbStatus status = csv_open(&csvReader, argv[1], &err);

    if (status != BDB_OK) {
        fprintf(stderr, "error: %s\n", err.message);
        return 1;
    }

    status = bdb_writer_open(&writer, "test-1.bdb", &err);

    if (status != BDB_OK) {
        fprintf(stderr, "error: %s\n", err.message);
        csv_close(&csvReader);
        return 1;
    }

    while ((status = csv_next_chunk(&csvReader, &chunk, &err)) == BDB_OK && chunk != NULL) {
        status = bdb_writer_append(&writer, chunk, &err);
        if (status != BDB_OK) break;
    }

    if (status == BDB_OK) {
        status = bdb_writer_finish(&writer, &err);
    }
    if (status != BDB_OK) fprintf(stderr, "error: %s\n", err.message);


    csv_close(&csvReader);
    bdb_writer_close(&writer);



    status = bdb_scan_open(&scan, "test-1.bdb", &err);
    bdb_filter_init(&filter, &scan.base, "QUANTITYORDERED", BDB_COMPARE_GE, "30");
    bdb_filter_init(&filter2, &filter.base, "QUANTITYORDERED", BDB_COMPARE_LT, "45");
    bdb_aggregate_init(&agg, &filter2.base, BDB_AGG_AVG, "QUANTITYORDERED");
    BdbOperator *op = &agg.base;

    while (status == BDB_OK && (status = bdb_op_next(op, &chunk, &err)) == BDB_OK && chunk != NULL) {
        if (chunk->count == 0) continue;
        print_chunk_cb(chunk, &err);
    }
    if (status != BDB_OK) fprintf(stderr, "error: %s\n", err.message);

    bdb_explain(op, stdout);

    op->close(op);


    return status == BDB_OK ? 0 : 1;


    // status = bdb_write("test.bdb", &table, &err);
    // if (status != BDB_OK) {
    //     fprintf(stderr, "error: %s\n", err.message);
    //     free_table(&table);
    //     return 1;
    // }
    // printf("\nWrote test.bdb\n");

    // // Read into a separate table so the CSV table isn't overwritten.
    // // bdb_read only loads the counts for now (no columns), so compare those
    // // instead of calling print_table/free_table on it.
    // TABLE loaded = {0};
    // status = bdb_read("test.bdb", &loaded, &err);
    // if (status != BDB_OK) {
    //     fprintf(stderr, "error: %s\n", err.message);
    //     free_table(&table);
    //     free_table(&loaded);
    //     return 1;
    // }
    //
    // printf("Read test.bdb: %" PRIu64 " rows, %" PRIu64 " columns\n",
    //        loaded.row_count, loaded.col_count);
    //
    // int matches = loaded.row_count == table.row_count &&
    //               loaded.col_count == table.col_count;
    // printf("Round trip: %s\n", matches ? "OK" : "MISMATCH");
    //
    // print_table(&loaded);
    //
    // size_t col_idx = 0;
    // int64_t result = 0;
    //
    // BdbQuery query = {
    //     BDB_AGG_COUNT,
    //     "price",
    //     true,
    //     {
    //         "year",
    //         BDB_OP_GE,
    //         2024
    //     }
    // };
    //
    // status = bdb_run_query(&loaded, &query, &result, &err);
    //
    // if (status != BDB_OK) {
    //     fprintf(stderr, "error: %s\n", err.message);
    //     free_table(&table);
    //     free_table(&loaded);
    //     return 1;
    // }
    //
    // printf("Sum of prices: %" PRId64 "\n", result);
    //

    //free_table(&loaded)
}


