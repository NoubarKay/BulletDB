#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/table.h"
#include "engine/common.h"

#define DELIMS ",\r\n"

static uint64_t get_total_rows(FILE *file) {
    uint64_t num_of_rows = 0;
    bool is_first_line = true;
    char buffer[256];

    while (fgets(buffer, sizeof buffer, file) != NULL) {
        if (is_first_line) {
            is_first_line = false;
            continue;
        }
        if (buffer[strspn(buffer, "\r\n")] == '\0') {
            continue;  // skip blank lines
        }
        num_of_rows++;
    }
    return num_of_rows;
}

static BdbStatus parse_header(TABLE *table, char *line, BdbError *err) {
    char *token = strtok(line, DELIMS);
    while (token != NULL) {
        COLUMN *temp = realloc(table->columns, sizeof(COLUMN) * (table->col_count + 1));
        if (temp == NULL) {
            return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory growing column list");
        }
        table->columns = temp;

        // Count the column BEFORE allocating its parts, so bdb_table_free
        // can clean it up if anything below fails.
        COLUMN *column = &table->columns[table->col_count];
        column->name = NULL;
        column->data = NULL;
        table->col_count++;

        column->name = strdup(token);
        if (column->name == NULL) {
            return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory for column name '%s'", token);
        }

        if (table->row_count > 0) {
            column->data = calloc(table->row_count, sizeof(int64_t));
            if (column->data == NULL) {
                return bdb_error_set(err, BDB_ERR_NOMEM,
                                     "out of memory for column '%s' data", column->name);
            }
        }

        token = strtok(NULL, DELIMS);
    }

    if (table->col_count == 0) {
        return bdb_error_set(err, BDB_ERR_PARSE, "line 1: header has no columns");
    }
    return BDB_OK;
}

static BdbStatus parse_row(TABLE *table, char *line, uint64_t row,
                           uint64_t line_no, BdbError *err) {
    char *token = strtok(line, DELIMS);

    for (uint64_t col = 0; col < table->col_count; col++) {
        if (token == NULL) {
            return bdb_error_set(err, BDB_ERR_PARSE,
                                 "line %llu: expected %llu values, got %llu",
                                 (unsigned long long)line_no,
                                 (unsigned long long)table->col_count,
                                 (unsigned long long)col);
        }

        char *end;
        long long value = strtoll(token, &end, 10);
        if (end == token || *end != '\0') {
            return bdb_error_set(err, BDB_ERR_PARSE,
                                 "line %llu: bad value '%s' in column '%s'",
                                 (unsigned long long)line_no, token,
                                 table->columns[col].name);
        }

        table->columns[col].data[row] = value;
        token = strtok(NULL, DELIMS);
    }

    if (token != NULL) {
        return bdb_error_set(err, BDB_ERR_PARSE,
                             "line %llu: more values than the %llu columns in the header",
                             (unsigned long long)line_no,
                             (unsigned long long)table->col_count);
    }
    return BDB_OK;
}

static BdbStatus read_table(TABLE *table, FILE *file, BdbError *err) {
    char buffer[256];
    uint64_t line_no = 0;
    uint64_t row = 0;

    while (fgets(buffer, sizeof buffer, file) != NULL) {
        line_no++;

        if (line_no == 1) {
            BdbStatus status = parse_header(table, buffer, err);
            if (status != BDB_OK) return status;
            continue;
        }

        if (buffer[strspn(buffer, "\r\n")] == '\0') {
            continue;  // skip blank lines
        }

        if (row >= table->row_count) {
            return bdb_error_set(err, BDB_ERR_PARSE,
                                 "file has more rows than counted (changed while reading?)");
        }

        BdbStatus status = parse_row(table, buffer, row, line_no, err);
        if (status != BDB_OK) return status;
        row++;
    }

    if (ferror(file)) {
        return bdb_error_set(err, BDB_ERR_IO, "read error at line %llu",
                             (unsigned long long)line_no);
    }
    return BDB_OK;
}

BdbStatus read_csv(const char *path, TABLE *table, BdbError *err) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return bdb_error_set(err, BDB_ERR_OPEN, "could not open '%s'", path);
    }

    table->row_count = get_total_rows(file);
    rewind(file);

    BdbStatus status = read_table(table, file, err);
    fclose(file);  // closed on both paths
    return status;
}