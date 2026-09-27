#include "csv/csv_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/table.h"
#include "common/common.h"
#include "csv/csv_tokenize.h"

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
    char *cursor = line;
    for (uint64_t col = 0; col < chunk->col_count; col++) {
        char *field = next_field(&cursor);
        if (field == NULL) {
            return bdb_error_set(err, BDB_ERR_PARSE,
                                 "line %llu: expected %llu values, got %llu",
                                 line_no, chunk->col_count, col);
        }

        if (strlen(field) > BDB_CSV_MAX_FIELD) {
            return bdb_error_set(err, BDB_ERR_PARSE,
                                 "line %llu: value in column '%s' is longer than %d characters",
                                 line_no, chunk->columns[col].name, BDB_CSV_MAX_FIELD);
        }

        chunk->columns[col].type = detect_file_type(field);
        chunk->columns[col].data = calloc(BDB_VECTOR_SIZE, bdb_col_type_size(chunk->columns[col].type));
        chunk->columns[col].bitmap = calloc(BDB_VECTOR_SIZE,sizeof(char));

        if (chunk->columns[col].data == NULL || chunk->columns[col].bitmap == NULL) {
            return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory for column '%s' data",
                                 chunk->columns[col].name);
        }
    }

    return BDB_OK;
}

static BdbStatus parse_row(CHUNK *chunk, char *line, uint64_t row, uint64_t line_no, BdbError *err) {
    char *cursor = line;
    for (uint64_t col = 0; col < chunk->col_count; col++) {

        char *field = next_field(&cursor);
        if (field == NULL) {
            return bdb_error_set(err, BDB_ERR_PARSE,
                                 "line %llu: expected %llu values, got %llu",
                                 line_no, chunk->col_count, col);
        }

        if (strlen(field) > BDB_CSV_MAX_FIELD) {
            return bdb_error_set(err, BDB_ERR_PARSE,
                                 "line %llu: value in column '%s' is longer than %d characters",
                                 line_no, chunk->columns[col].name, BDB_CSV_MAX_FIELD);
        }

        COLUMN *column = &chunk->columns[col];

        // An empty field is NULL: store a zero value and mark it invalid.
        bool is_null = field[0] == '\0';
        column->bitmap[row] = is_null ? 0 : 1;

        switch (column->type) {
            case BDB_COL_INT:
                ((int64_t *)column->data)[row] = is_null ? 0 : (int64_t)strtoll(field, NULL, 10);
                break;
            case BDB_COL_DOUBLE:
                ((double *)column->data)[row] = is_null ? 0.0 : strtod(field, NULL);
                break;
            case BDB_COL_BOOL:
                // Accepts "true"/"t"/"yes"/"1" as true, anything else as false
                ((bool *)column->data)[row] = !is_null &&
                    (strcasecmp(field, "true") == 0 || strcasecmp(field, "t") == 0 ||
                     strcasecmp(field, "yes") == 0  || strcmp(field, "1") == 0);
                break;
            default:
                return bdb_error_set(err, BDB_ERR_PARSE,
                                     "line %llu: unsupported column type %d",
                                     line_no,
                                     column->type);
        }
    }

    if (next_field(&cursor) != NULL) {
        return bdb_error_set(err, BDB_ERR_PARSE,
                             "line %llu: more values than the %llu columns in the header",
                             line_no, chunk->col_count);
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

    char* dest = NULL;


    status = read_next_line(reader->file, reader->buffer, err, &reader->line_no, &dest);
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
        if (line == NULL) break; // EOF
        if (line[0] == '\0') continue; // blank line

        if (!r->have_types) {
            // next_field cuts the line up in place, so detect types on a
            // copy and leave the original for parse_row. Runs once per file.
            char *copy = strdup(line);
            if (copy == NULL) {
                return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory copying line %llu",
                                     r->line_no);
            }
            status = detect_types_allo_chunk_buffers(&r->chunk, copy, r->line_no, err);
            free(copy);
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
