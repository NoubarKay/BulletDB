#include <stdio.h>
#include <stdlib.h>

#include "format.h"
#include "io.h"
#include "../common.h"
#include "../table.h"

/*
 * BulletDB
 *
 * Copyright (c) 2026 Noubar Kassabian
 *
 * This source file is part of BulletDB, a columnar
 * binary database engine written in C.
 */

/**
 * Reads the table header from a binary database file.
 *
 * The header is stored as a fixed 20-byte structure:
 *
 * <pre>
 * Offset   Size   Field
 * ------   ----   ----------------
 * 0x00      8     row_count  (uint64)
 * 0x08      8     col_count  (uint64)
 * 0x10      4     group_size (uint32)
 * ------   ----   ----------------
 * 0x14     20     Total header size
 * </pre>
 *
 * The values are validated before being stored in the table.
 *
 * @param file  File stream to read the header from.
 * @param table Table structure to populate with the header values.
 * @param err   Error structure populated if the operation fails.
 * @param column_count Pointer to store the column count read from the header.
 *
 * @return BDB_OK on success, or an appropriate BdbStatus error code
 *         if the header cannot be read or contains invalid values.
 */
static BdbStatus bdb_read_header(FILE *file, TABLE *table, BdbError *err, uint64_t *column_count) {
    BdbStatus status = BDB_OK;

    uint64_t row_count = 0, col_count = 0;
    uint32_t group_size = 0;
    status = read_bytes(file, &row_count, sizeof(uint64_t), 1, err, "could not read row count from file");
    if (status != BDB_OK) {
        return status;
    }

    status = read_bytes(file, &col_count, sizeof(uint64_t), 1, err, "could not read col count from file");
    if (status != BDB_OK) {
        return status;
    }

    if (col_count == 0) {
        return bdb_error_set(err, BDB_ERR_FORMAT, "col count is zero");
    }
    if (col_count > BDB_MAX_COL_COUNT) {
        return bdb_error_set(err, BDB_ERR_FORMAT, "col count is too large");
    }

    status = read_bytes(file, &group_size, sizeof(uint32_t), 1, err, "could not read group size from file");
    if (status != BDB_OK) {
        return status;
    }
    if (group_size == 0) {
        return bdb_error_set(err, BDB_ERR_FORMAT, "group size is zero");
    }
    if (group_size > BDB_GROUP_SIZE) {
        return bdb_error_set(err, BDB_ERR_FORMAT, "group size is invalid");
    }


    table->row_count = row_count;
    *column_count = col_count;
    table->group_size = group_size;

    return BDB_OK;
}

/**
 * Reads column names from a binary database file.
 *
 * Each column name is stored using the following format:
 *
 * <pre>
 * Offset   Size              Field
 * ------   ----------------  ----------------------
 * +0x00     4                name_length (uint32)
 * +0x04     name_length      name (char[])
 * </pre>
 *
 * This structure is repeated once for each column in the table.
 * The column name is null-terminated in memory after being read,
 * but the null terminator is not stored in the file.
 *
 * @param col_count Number of column names to read from the file.
 * @param table Table structure to populate with the column names.
 * @param err Error structure populated if the operation fails.
 * @param file File stream to read the column names from.
 *
 * @return BDB_OK on success, or an appropriate BdbStatus error code
 *         if memory allocation fails or the column data cannot be read.
 */
static BdbStatus bdb_read_col_names(const uint64_t col_count, TABLE* table, BdbError *err, FILE *file) {

    BdbStatus status = BDB_OK;

    for (size_t i = 0; i < col_count; i++) {
        COLUMN *temp = realloc(table->columns, sizeof(COLUMN) * (table->col_count + 1));
        if (temp == NULL) {
            return bdb_error_set(err, BDB_ERR_NOMEM, "could not allocate memory for column");
        }
        table->columns = temp;
        table->columns[i].data = NULL;
        uint32_t name_length = 0;

        status = read_bytes(file, &name_length, sizeof(uint32_t), 1, err, "could not read column name length");
        if (status != BDB_OK) {
            return status;
        }
        char *name = malloc(name_length + 1);
        if (name == NULL) {
            return bdb_error_set(err, BDB_ERR_NOMEM, "could not allocate memory for column name");
        }
        status = read_bytes(file, name, sizeof(char), name_length, err, "could not read column name");
        if (status != BDB_OK) {
            free(name);
            return status;
        }
        name[name_length] = '\0';
        table->columns[i].name = name;
        table->col_count++;
    }

    return BDB_OK;
}

/**
 * Reads row data from a binary database file in groups.
 *
 * Column data is stored in columnar row groups. Each row group contains
 * up to `table->group_size` rows, with values for each column stored
 * sequentially.
 *
 * The on-disk structure is:
 *
 * <pre>
 * Row Group 0:
 *     Column 0: [row 0] [row 1] ... [row N]
 *     Column 1: [row 0] [row 1] ... [row N]
 *     ...
 *     Column M: [row 0] [row 1] ... [row N]
 *
 * Row Group 1:
 *     Column 0: [row N+1] ...
 *     Column 1: [row N+1] ...
 *     ...
 * </pre>
 *
 * Each value is stored as an int64_t. Memory for every column is allocated
 * for the table's complete row count before reading the row groups.
 *
 * @param table Table structure containing the column definitions, row count,
 *              and row group size. Populated with the row data.
 * @param file File stream to read the row groups from.
 * @param err Error structure populated if the operation fails.
 *
 * @return BDB_OK on success, or an appropriate BdbStatus error code
 *         if row data cannot be read.
 */
static BdbStatus bdb_read_row_groups(TABLE *table, FILE *file, BdbError *err) {
    size_t start = 0;
    BdbStatus status = BDB_OK;

    for (size_t i = 0; i < table->col_count; i++) {
        table->columns[i].data = realloc(table->columns[i].data, sizeof(int64_t) * (table->row_count));
        if (table->columns[i].data == NULL) {
            return bdb_error_set(err, BDB_ERR_NOMEM, "could not allocate memory for column");
        }
    }

    while (start < table->row_count) {
        size_t rows_left = table->row_count - start;
        size_t size = rows_left > table->group_size ? table->group_size : rows_left;
        for (size_t i = 0; i < table->col_count; i++) {
            status = read_bytes(file, table->columns[i].data + start, sizeof(int64_t), size, err, "could not read row group");
            if (status != BDB_OK) {
                return status;
            }
        }

        start += table->group_size;
    }

    return BDB_OK;
}

/**
 * Reads a BulletDB table from a binary database file.
 *
 * The file is read in three stages:
 *
 * <pre>
 * 1. Header
 *    ├── row_count
 *    ├── col_count
 *    └── group_size
 *
 * 2. Column metadata
 *    └── column names
 *
 * 3. Row data
 *    └── columnar row groups
 * </pre>
 *
 * On success, the supplied TABLE is populated with the table metadata,
 * column definitions, and row data.
 *
 * The file is always closed before this function returns.
 *
 * @param path Path to the binary database file.
 * @param table Table structure to populate with the data read from the file.
 * @param err Error structure populated if the operation fails.
 *
 * @return BDB_OK on success, or an appropriate BdbStatus error code if
 *         the file cannot be opened, read, or contains invalid data.
 */
BdbStatus bdb_read(const char *path, TABLE *table, BdbError *err) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return bdb_error_set(err, BDB_ERR_OPEN, "could not open '%s'", path);
    }

    BdbStatus status = BDB_OK;
    uint64_t column_count = 0;

    status = bdb_read_header(file, table, err, &column_count);
    if (status != BDB_OK)
        goto cleanup;


    status = bdb_read_col_names(column_count, table, err, file);
    if (status != BDB_OK)
        goto cleanup;

    if (table->row_count > 0) {
        status = bdb_read_row_groups(table, file, err);
        if (status != BDB_OK)
            goto cleanup;
    }


    cleanup:
        if (fclose(file) != 0 && status == BDB_OK) {
            status = bdb_error_set(err, BDB_ERR_IO, "could not finish reading '%s'", path);
        }

    return status;
}

