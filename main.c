//
// Created by user on 9/24/2026.
//

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/engine/table.h"
#include "src/csv.h"
#include "src/engine/common.h"
#include "src/engine/query/query.h"
#include "src/engine/storage/bdb.h"


int main(int argc, char *argv[]) {
    BdbError err = {0};

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file.csv>\n", argv[0]);
        return 1;
    }

    TABLE table = {
        .col_count = 0,
        .row_count = 0,
        .group_size = 2,
        .columns = NULL
    };

    BdbStatus status = read_csv(argv[1], &table, &err);

    if (status != BDB_OK) {
        fprintf(stderr, "error: %s\n",
                err.message[0] != '\0' ? err.message : bdb_status_str(status));
        free_table(&table);
        return 1;
    }

    if (table.col_count == 0) {
        fprintf(stderr, "No columns read from %s\n", argv[1]);
        return 1;
    }

    print_table(&table);


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

    free_table(&table);
    //free_table(&loaded);
    return 0;
}


