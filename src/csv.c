#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/table.h"
#include "engine/common.h"

#define DELIMS ",\r\n"
#define BDB_CSV_MAX_LINE (1024 * 1024)
#define BDB_CSV_MAX_FIELD 255

static uint64_t get_total_rows(FILE *file, char *buffer) {
    uint64_t num_of_rows = 0;
    bool is_first_line = true;

    while (fgets(buffer, BDB_CSV_MAX_LINE, file) != NULL) {
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

        token = strtok(NULL, DELIMS);
    }

    if (table->col_count == 0) {
        return bdb_error_set(err, BDB_ERR_PARSE, "line 1: header has no columns");
    }
    return BDB_OK;
}

void trim_string(const char *src, char *dst) {
    while (isspace((unsigned char)*src)) src++;
    size_t len = strlen(src);
    while (len > 0 && isspace((unsigned char)src[len - 1])) len--;
    strncpy(dst, src, len);
    dst[len] = '\0';
}

static enum ColumnType detect_file_type(const char *entry) {
    char str[256];
    trim_string(entry, str);

    if (strcasecmp(str, "true") == 0 || strcasecmp(str, "false") == 0 ||
    strcasecmp(str, "t") == 0    || strcasecmp(str, "f") == 0 ||
    strcasecmp(str, "yes") == 0  || strcasecmp(str, "no") == 0) {
        return BDB_COL_BOOL;
    }

    char *endptr;
    strtol(entry, &endptr, 10);
    while (isspace((unsigned char)*endptr)) endptr++;
    if (*endptr == '\0') {
        return BDB_COL_INT;
    }

    strtod(entry, &endptr);
    while (isspace((unsigned char)*endptr)) endptr++;
    if (*endptr == '\0') {
        return BDB_COL_DOUBLE;
    }

    return BDB_COL_STR;
}

static BdbStatus parse_row(TABLE *table, char *line, uint64_t row,
                           uint64_t line_no, BdbError *err) {
    char *token = strtok(line, DELIMS);

    for (uint64_t col = 0; col < table->col_count; col++) {
        if (token == NULL) {
            return bdb_error_set(err, BDB_ERR_PARSE,
                                 "line %llu: expected %llu values, got %llu",
                                 line_no,
                                 table->col_count,
                                 col);
        }

        char *end;

        if (strlen(token) > BDB_CSV_MAX_FIELD) {
            return bdb_error_set(err, BDB_ERR_PARSE,
                                 "line %llu: value in column '%s' is longer than %d characters",
                                 line_no, table->columns[col].name, BDB_CSV_MAX_FIELD);
        }

        if (row == 0) {

            table->columns[col].type = detect_file_type(token);
            table->columns[col].data = calloc(table->row_count,
                                              bdb_col_type_size(table->columns[col].type));
            if (table->columns[col].data == NULL) {
                return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory for column '%s' data",
                                     table->columns[col].name);
            }
        }

        switch (table->columns[col].type) {
            case BDB_COL_INT:    ((int64_t *)table->columns[col].data)[row] = (int64_t)strtoll(token, &end, 10); break;
            case BDB_COL_DOUBLE: ((double  *)table->columns[col].data)[row] = (double)strtod(token, &end); break;
            case BDB_COL_BOOL: {
                // Safely handles text ("true"/"false", "T"/"F", "yes"/"no") or digits ("1"/"0")
                bool bool_val = false;
                if (strcasecmp(token, "true") == 0 || strcasecmp(token, "t") == 0 ||
                    strcasecmp(token, "yes") == 0  || strcmp(token, "1") == 0) {
                    bool_val = true;
                    } else {
                        bool_val = false;
                    }
                ((bool *)table->columns[col].data)[row] = bool_val;
                break;
            }        }


        token = strtok(NULL, DELIMS);
    }

    if (token != NULL) {
        return bdb_error_set(err, BDB_ERR_PARSE,
                             "line %llu: more values than the %llu columns in the header",
                             line_no,
                             table->col_count);
    }
    return BDB_OK;
}

static BdbStatus read_table(TABLE *table, FILE *file, BdbError *err, char * buffer) {
    uint64_t line_no = 0;
    uint64_t row = 0;

    while (fgets(buffer, BDB_CSV_MAX_LINE, file) != NULL) {
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
    char *buffer = malloc(BDB_CSV_MAX_LINE);
    if (buffer == NULL) {
        free(buffer);
        return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory for line buffer");
    }
    table->row_count = get_total_rows(file, buffer);
    rewind(file);

    BdbStatus status = read_table(table, file, err, buffer);
    fclose(file);  // closed on both paths
    free(buffer);
    return status;
}