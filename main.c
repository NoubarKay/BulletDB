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
        .columns = chunk->columns
    };
    print_table(&view);
    return BDB_OK;
}

int main(int argc, char *argv[]) {
    BdbError err = {0};
    CSV_READER csvReader = {0};
    BDB_WRITER writer = {0};
    BdbScan scan;
    BdbAggregate agg;

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
    bdb_aggregate_init(&agg, &scan.base, BDB_AGG_COUNT, "PRICEEACH");
    BdbOperator *op = &agg.base;                 // main only talks to the top operator


    bdb_explain(op, stdout);


    while (status == BDB_OK && (status = op->next(op, &chunk, &err)) == BDB_OK && chunk != NULL) {
        print_chunk_cb(chunk, &err);             // prints the 1-row result table
    }
    if (status != BDB_OK) fprintf(stderr, "error: %s\n", err.message);
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


