#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "format.h"
#include "../common.h"
#include "../table.h"
#include "./io.h"
//
// Created by user on 9/24/2026.
//

static void write_row_groups(FILE *file, TABLE *table) {
    int start = 0;

    while (start < table->row_count) {
        int rows_left = table->row_count - start;
        int size = rows_left > table->group_size ? table->group_size : rows_left;
        for (int i = 0; i < table->col_count; i++) {
            fwrite(table->columns[i].data + start, sizeof(int64_t), size, file);
        }

        start += table->group_size;
    }
}



BdbStatus bdb_write(const char *path, const TABLE *table, BdbError *err) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return bdb_error_set(err, BDB_ERR_OPEN, "could not open '%s'", path);
    }

    BdbStatus status = BDB_OK;

    if (fwrite(&table->row_count, sizeof(uint64_t), 1, f) != 1) {
        status = bdb_error_set(err, BDB_ERR_IO, "could not write row count");
        goto cleanup;
    }
    if (fwrite(&table->col_count, sizeof(uint64_t), 1, f) != 1) {
        status = bdb_error_set(err, BDB_ERR_IO, "could not write column count");
        goto cleanup;
    }
    if (fwrite(&table->group_size, sizeof(uint32_t), 1, f) != 1) {
        status = bdb_error_set(err, BDB_ERR_IO, "could not write group count");
        goto cleanup;
    }


    // Header: each column's name, as a uint32 length followed by the bytes
    for (uint64_t c = 0; c < table->col_count; c++) {
        const char *name = table->columns[c].name;
        uint32_t name_length = (uint32_t)strlen(name);

        if (fwrite(&name_length, sizeof(uint32_t), 1, f) != 1) {
            status = bdb_error_set(err, BDB_ERR_IO,
                                   "could not write name length of column '%s'", name);
            goto cleanup;
        }
        if (fwrite(name, 1, name_length, f) != name_length) {
            status = bdb_error_set(err, BDB_ERR_IO,
                                   "could not write name of column '%s'", name);
            goto cleanup;
        }
    }

    write_row_groups(f, table);

    // Data: each column as one contiguous block
    // for (uint64_t c = 0; c < table->col_count; c++) {
    //     size_t written = fwrite(table->columns[c].data, sizeof(int64_t),
    //                             (size_t)table->row_count, f);
    //     if (written != table->row_count) {
    //         status = bdb_error_set(err, BDB_ERR_IO,
    //                                "could not write data of column '%s'",
    //                                table->columns[c].name);
    //         goto cleanup;
    //     }
    // }

    cleanup:
        if (fclose(f) != 0 && status == BDB_OK) {
            status = bdb_error_set(err, BDB_ERR_IO, "could not finish writing '%s'", path);
        }
    return status;
}

