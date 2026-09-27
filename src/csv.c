#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/table.h"
#include "engine/common.h"
#include "engine/sink/bdb_sink.h"
#include "engine/storage/format.h"
#include "engine/csv/csv_tokenize.h"

static BdbStatus parse_header(CHUNK* chunk, char *line, BdbError *err) {
    char *token = strtok(line, DELIMS);
    while (token != NULL) {
        COLUMN *temp = realloc(chunk->columns, sizeof(COLUMN) * (chunk->col_count + 1));
        if (temp == NULL) {
            return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory growing column list");
        }
        chunk->columns = temp;

        // Count the column BEFORE allocating its parts, so bdb_table_free
        // can clean it up if anything below fails.
        COLUMN *column = &chunk->columns[chunk->col_count];
        column->name = NULL;
        column->data = NULL;
        chunk->col_count++;

        column->name = strdup(token);
        if (column->name == NULL) {
            return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory for column name '%s'", token);
        }

        token = strtok(NULL, DELIMS);
    }

    if (chunk->col_count == 0) {
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

static BdbStatus detect_types_allo_chunk_buffers(CHUNK* chunk, char *line, uint64_t line_no, BdbError *err) {
    char *token;
    char *moving_line = line;
    token = extract_value(&moving_line);

    while (token != NULL) {
        for (uint64_t col = 0; col < chunk->col_count; col++) {
            if (token == NULL) {
                continue;
            }

            if (token != NULL && strlen(token) > BDB_CSV_MAX_FIELD) {
                return bdb_error_set(err, BDB_ERR_PARSE,
                                     "line %llu: value in column '%s' is longer than %d characters",
                                     line_no, chunk->columns[col].name, BDB_CSV_MAX_FIELD);
            }

            chunk->columns[col].type = detect_file_type(token);
            chunk->columns[col].data = calloc(BDB_VECTOR_SIZE, bdb_col_type_size(chunk->columns[col].type));
            chunk->columns[col].bitmap = calloc(BDB_VECTOR_SIZE,sizeof(char));

            if (chunk->columns[col].data == NULL) {
                return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory for column '%s' data",
                                     chunk->columns[col].name);
            }

            token = extract_value(&moving_line);
        }
    }

    return BDB_OK;
}

static BdbStatus parse_row(CHUNK *chunk, char *line, uint64_t row, uint64_t line_no, BdbError *err) {
    char *token;
    char *moving_line = line;
    token = extract_value(&moving_line);

    while (token != NULL) {
        for (uint64_t col = 0; col < chunk->col_count; col++) {

            char *end;

            if (token == NULL) {
                continue;
            }

            if (token != NULL && strlen(token) > BDB_CSV_MAX_FIELD) {
                return bdb_error_set(err, BDB_ERR_PARSE,
                                     "line %llu: value in column '%s' is longer than %d characters",
                                     line_no, chunk->columns[col].name, BDB_CSV_MAX_FIELD);
            }

            switch (chunk->columns[col].type) {
                case BDB_COL_INT:
                    ((int64_t *)chunk->columns[col].data)[row] = (int64_t)strtoll(strcmp(token, "") != 0 ? token : "0", &end, 10);
                    if (strcmp(token, "") != 0) {
                        chunk->columns[col].bitmap[row] = 1;
                    }else {
                        chunk->columns[col].bitmap[row] = 0;
                    }
                    break;
                case BDB_COL_DOUBLE:
                    ((double  *)chunk->columns[col].data)[row] = (double)strtod(strcmp(token, "") != 0 ? token : "0", &end);
                    if (strcmp(token, "") != 0) {
                        chunk->columns[col].bitmap[row] = 1;
                    }else {
                        chunk->columns[col].bitmap[row] = 0;
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
                    ((bool *)chunk->columns[col].data)[row] = bool_val;
                    if (token != NULL) {
                        chunk->columns[col].bitmap[row] = 1;
                    }else {
                        chunk->columns[col].bitmap[row] = 0;
                    }
                    break;
                }
                default:
                    return bdb_error_set(err, BDB_ERR_PARSE,
                                         "line %llu: unsupported column type %d",
                                         line_no,
                                         chunk->columns[col].type);
            }


            token = extract_value(&moving_line);
        }

        if (token != NULL && strcmp(token, "") != 0 ) {
            return bdb_error_set(err, BDB_ERR_PARSE,
                                 "line %llu: more values than the %llu columns in the header",
                                 line_no,
                                 chunk->col_count);
        }

    }
    return BDB_OK;
}

BdbStatus read_csv(const char *path, BdbChunkFn on_chunk, BdbError *err) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return bdb_error_set(err, BDB_ERR_OPEN, "could not open '%s'", path);
    }
    char *buffer = malloc(BDB_CSV_MAX_LINE);
    if (buffer == NULL) {
        free(buffer);
        return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory for line buffer");
    }

    BdbStatus status = BDB_OK;
    uint64_t line_no = 0;
    uint64_t row = 0;
    char* dest = NULL;

    CHUNK chunk = {
        .col_count = 0,
        .count = 0,
        .columns = NULL
    };

    while (true) {
        status = read_next_line(file, buffer, err, &line_no, &dest);
        if (status != BDB_OK) break;

        if (dest == NULL) {
            break;
        }

        if (line_no == 1) {
            parse_header(&chunk, buffer, err);
            continue;
        }

        if (buffer[strspn(buffer, "\r\n")] == '\0') {
            continue;  // skip blank lines
        }

        if (line_no == 2) {
            status = detect_types_allo_chunk_buffers(&chunk, buffer, line_no, err);
        }

        status = parse_row(&chunk, buffer, chunk.count, line_no, err);
        chunk.count++;

        if (chunk.count == BDB_VECTOR_SIZE) {
            on_chunk(&chunk, NULL, err);
            chunk_reset(&chunk);
        }
    }

    if (chunk.count > 0){
        on_chunk(&chunk, NULL, err);
        chunk_reset(&chunk);
    }

    // BdbStatus status = get_total_rows(file, buffer, &table->row_count, err);
    // if (status != BDB_OK) { fclose(file); free(buffer); return status; }
    // rewind(file);
    //
    // status = read_table(table, file, err, buffer);
    fclose(file);  // closed on both paths
    free(buffer);
    return status;
}