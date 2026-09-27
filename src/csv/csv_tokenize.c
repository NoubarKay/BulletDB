#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/common.h"
#include "csv/csv_tokenize.h"

static void trim_trailing(char *src) {
    size_t length = strlen(src);
    while (length > 0 && isspace((unsigned char)src[length - 1])) length--;
    src[length] = '\0';
}

char *next_field(char **cursor) {
    char *start = *cursor;
    if (start == NULL) return NULL;

    char *end;
    char *comma = strchr(start, ',');
    if (comma != NULL) {
        *comma = '\0';
        *cursor = comma + 1;
        end = comma;
    } else {
        end = start + strlen(start);
        *cursor = NULL;
    }

    while (start < end && isspace((unsigned char)*start)) start++;
    while (end > start && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return start;
}

BdbStatus read_next_line(FILE *file, char *buffer, BdbError *err, uint64_t *line_no, char** dest) {
    char *buffer_result = fgets(buffer, BDB_CSV_MAX_LINE, file);
    *dest = NULL;

    if (buffer_result == NULL && ferror(file)) {
        return bdb_error_set(err, BDB_ERR_IO,
                                 "read error at line %" PRIu64, *line_no + 1);
    }

    if (buffer_result == NULL && feof(file)) {
        *dest = NULL;
        return BDB_OK;
    }

    (*line_no)++;

    if (!feof(file) && buffer_result && buffer_result[strlen(buffer) -1] != '\n') {
        return bdb_error_set(err, BDB_ERR_PARSE,
                                 "line %" PRIu64 " exceeds the maximum length of %d bytes", *line_no, BDB_CSV_MAX_LINE);
    }

    trim_trailing(buffer);
    *dest = buffer;
    return BDB_OK;
}