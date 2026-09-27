#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/table.h"
#include "engine/common.h"
#include "engine/storage/format.h"
#include "engine/csv/csv_tokenize.h"


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



static enum ColumnType detect_file_type(const char *entry) {

    // 2. Guard against an empty string after trimming
    if (entry[0] == '\0') {
        return BDB_COL_STR;
    }

    // Boolean Check
    if (strcasecmp(entry, "true") == 0  || strcasecmp(entry, "false") == 0 ||
        strcasecmp(entry, "t") == 0     || strcasecmp(entry, "f") == 0     ||
        strcasecmp(entry, "yes") == 0   || strcasecmp(entry, "no") == 0) {
        return BDB_COL_BOOL;
        }

    char *endptr;

    // Integer Check (Passing 'str' instead of 'entry')
    strtol(entry, &endptr, 10);
    // Ensure endptr actually moved (endptr != str) and reached the end of the string
    if (endptr != entry && *endptr == '\0') {
        return BDB_COL_INT;
    }

    // Double Check (Passing 'str' instead of 'entry')
    strtod(entry, &endptr);
    // Ensure endptr actually moved (endptr != str) and reached the end of the string
    if (endptr != entry && *endptr == '\0') {
        return BDB_COL_DOUBLE;
    }

    return BDB_COL_STR;
}



static BdbStatus parse_row(TABLE *table, char *line, uint64_t row,
                           uint64_t line_no, BdbError *err) {
    char *token;
    char *moving_line = line;
    token = extract_value(&moving_line);

    while (token != NULL) {
        for (uint64_t col = 0; col < table->col_count; col++) {

            char *end;

            if (token == NULL) {
                continue;
            }

            if (token != NULL && strlen(token) > BDB_CSV_MAX_FIELD) {
                return bdb_error_set(err, BDB_ERR_PARSE,
                                     "line %llu: value in column '%s' is longer than %d characters",
                                     line_no, table->columns[col].name, BDB_CSV_MAX_FIELD);
            }

            if (row == 0) {

                table->columns[col].type = detect_file_type(token);
                table->columns[col].data = calloc(table->row_count,
                                                  bdb_col_type_size(table->columns[col].type));
                table->columns[col].bitmap = calloc(table->row_count,sizeof(char));

                if (table->columns[col].data == NULL) {
                    return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory for column '%s' data",
                                         table->columns[col].name);
                }
            }

            switch (table->columns[col].type) {
                case BDB_COL_INT:
                    ((int64_t *)table->columns[col].data)[row] = (int64_t)strtoll(strcmp(token, "") != 0 ? token : "0", &end, 10);
                    if (strcmp(token, "") != 0) {
                        table->columns[col].bitmap[row] = 1;
                    }else {
                        table->columns[col].bitmap[row] = 0;
                    }
                    break;
                case BDB_COL_DOUBLE:
                    ((double  *)table->columns[col].data)[row] = (double)strtod(strcmp(token, "") != 0 ? token : "0", &end);
                    if (strcmp(token, "") != 0) {
                        table->columns[col].bitmap[row] = 1;
                    }else {
                        table->columns[col].bitmap[row] = 0;
                    }
                    break;
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
                    if (token != NULL) {
                        table->columns[col].bitmap[row] = 1;
                    }else {
                        table->columns[col].bitmap[row] = 0;
                    }
                    break;
                }
                default:
                    return bdb_error_set(err, BDB_ERR_PARSE,
                                         "line %llu: unsupported column type %d",
                                         line_no,
                                         table->columns[col].type);
            }


            token = extract_value(&moving_line);
        }

        if (token != NULL && strcmp(token, "") != 0 ) {
            return bdb_error_set(err, BDB_ERR_PARSE,
                                 "line %llu: more values than the %llu columns in the header",
                                 line_no,
                                 table->col_count);
        }

    }
    return BDB_OK;
}

static BdbStatus read_table(TABLE *table, FILE *file, BdbError *err, char * buffer) {
    uint64_t line_no = 0;
    uint64_t row = 0;
    char* dest = NULL;
    BdbStatus status = BDB_OK;


    while (true) {
        status = read_next_line(file, buffer, err, &line_no, &dest);
        if (status != BDB_OK) return status;

        if (line_no == 1) {
            BdbStatus status = parse_header(table, buffer, err);
            if (status != BDB_OK) return status;
            continue;
        }

        if (buffer[strspn(buffer, "\r\n")] == '\0') {
            continue;  // skip blank lines
        }

        if (dest == NULL) {
            break;
        }

        if (row > table->row_count) {
            return bdb_error_set(err, BDB_ERR_PARSE,
                                 "file has more rows than counted (changed while reading?)");
        }

        status = parse_row(table, buffer, row, line_no, err);
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