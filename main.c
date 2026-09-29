//
// Created by user on 9/24/2026.
//
#define _CRTDBG_MAP_ALLOC
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/table.h"
#include "csv/csv_reader.h"
#include "common/common.h"
#include "query/query.h"
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
    BDB_READER reader = {0};

    const CHUNK *chunk;
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file.csv>\n", argv[0]);
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

    status = bdb_reader_open(&reader, "test-1.bdb", &err);
    if (status != BDB_OK) {
        fprintf(stderr, "error: %s\n", err.message);
    }


    if (status == BDB_OK) {
        uint64_t chunks_read = 0;
        uint64_t rows_read = 0;

        while ((status = bdb_reader_next_chunk(&reader, &chunk, &err)) == BDB_OK && chunk != NULL) {
            chunks_read++;
            rows_read += chunk->count;
            // print_chunk_cb(chunk, &err);   // uncomment to see each chunk's rows
        }
        if (status != BDB_OK) fprintf(stderr, "error: %s\n", err.message);

        printf("\nreader streamed %" PRIu64 " chunks, %" PRIu64 " rows (footer says %" PRIu64 ")\n",
               chunks_read, rows_read, reader.row_count);
    }

    bdb_reader_close(&reader);

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


