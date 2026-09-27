#include "csv.h"

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
        column->bitmap = NULL;
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

            if (strlen(token) > BDB_CSV_MAX_FIELD) {
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

            if (strlen(token) > BDB_CSV_MAX_FIELD) {
                return bdb_error_set(err, BDB_ERR_PARSE,
                                     "line %llu: value in column '%s' is longer than %d characters",
                                     line_no, chunk->columns[col].name, BDB_CSV_MAX_FIELD);
            }

            switch (chunk->columns[col].type) {
                case BDB_COL_INT:
                    ((int64_t *)chunk->columns[col].data)[row] = (int64_t)strtoll( token[0] == '\0' ? token : "0", &end, 10);
                    if ( token[0] == '\0') {
                        chunk->columns[col].bitmap[row] = 1;
                    }else {
                        chunk->columns[col].bitmap[row] = 0;
                    }
                    break;
                case BDB_COL_DOUBLE:
                    ((double  *)chunk->columns[col].data)[row] = (double)strtod( token[0] == '\0' ? token : "0", &end);
                    if ( token[0] == '\0') {
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
                    if ((bool *)bool_val != NULL) {
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

        if (token != NULL &&  token[0] == '\0' ) {
            return bdb_error_set(err, BDB_ERR_PARSE,
                                 "line %llu: more values than the %llu columns in the header",
                                 line_no,
                                 chunk->col_count);
        }

    }
    return BDB_OK;
}

BdbStatus csv_open(CSV_READER *reader, const char *path, BdbError *err) {
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        return bdb_error_set(err, BDB_ERR_OPEN, "could not open '%s'", path);
    }
    reader->file = file;
    reader->buffer = malloc(BDB_CSV_MAX_LINE);

    if (reader->buffer == NULL) {
        free(reader->buffer);
        return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory for line buffer");
    }

    CHUNK chunk = {
        .col_count = 0,
        .count = 0,
        .columns = NULL
    };

    reader->chunk = chunk;

    chunk_init(&reader->chunk, 0, err);
    BdbStatus status = BDB_OK;

    uint64_t line_no = 0;
    char* dest = NULL;


    status = read_next_line(reader->file, reader->buffer, err, &line_no, &dest);
    if (status != BDB_OK) return status;

    if (dest == NULL) {
        return bdb_error_set(err, BDB_ERR_PARSE, "line 1: header has no columns");
    }
    parse_header(&reader->chunk, reader->buffer, err);
    return status;
}

BdbStatus csv_next_chunk(CSV_READER *r, const CHUNK **out, BdbError *err) {
    *out = NULL;
    chunk_reset(&r->chunk);
    char *line = NULL;

    while (r->chunk.count < BDB_VECTOR_SIZE) {
        BdbStatus status = read_next_line(r->file, r->buffer, err, &r->line_no, &line);
        if (status != BDB_OK) return status;
        if (line == NULL) break;            // EOF
        if (line[0] == '\0') continue;      // blank line

        if (!r->have_types) {
            status = detect_types_allo_chunk_buffers(&r->chunk, line, r->line_no, err);
            if (status != BDB_OK) return status;
            r->have_types = true;
        }

        status = parse_row(&r->chunk, line, r->chunk.count, r->line_no, err);
        if (status != BDB_OK) return status;
        r->chunk.count++;
    }

    if (r->chunk.count > 0) *out = &r->chunk;
    return BDB_OK;
}

void csv_close(CSV_READER *r) {
    chunk_free(&r->chunk);
    if (r->file != NULL) fclose(r->file);
    free(r->buffer);
    r->file = NULL;
    r->buffer = NULL;
    r->have_types = false;
    r->line_no = 0;
}
