//
// Created by user on 9/24/2026.
//
#include "table.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

void print_table(TABLE *table) {
    for (uint64_t col = 0; col < table->col_count; col++) {
        printf("%-12s", table->columns[col].name);
    }
    printf("\n");

    for (uint64_t col = 0; col < table->col_count; col++) {
        printf("------------");
    }
    printf("\n");

    for (uint64_t row = 0; row < table->row_count; row++) {
        for (uint64_t col = 0; col < table->col_count; col++) {
            printf("%-12" PRId64, table->columns[col].data[row]);
        }
        printf("\n");
    }

    printf("(%" PRIu64 " rows, %" PRIu64 " columns)\n", table->row_count, table->col_count);
}

void free_table(TABLE *table)
{
    for (uint64_t col = 0; col < table->col_count; col++) {
        free(table->columns[col].name);
        free(table->columns[col].data);
    }

    free(table->columns);

    table->columns = NULL;
    table->col_count = 0;
    table->row_count = 0;
}

BdbStatus bdb_find_column(const TABLE *table, const char *name, size_t *col_idx, BdbError *err) {
    for (size_t col = 0; col < table->col_count; col++) {
        if (strcmp(table->columns[col].name, name) == 0) {
            *col_idx = col;
            return BDB_OK;
        }
    }

    return bdb_error_set(err, BDB_ERR_NOT_FOUND, "%s", "The column was not found");
}