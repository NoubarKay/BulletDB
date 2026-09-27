#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../storage/format.h"
#include "../common.h"
//
// Created by user on 9/26/2026.
//

static void trim_string(const char *src, char *dst) {
    if (src == NULL) {
        return;
    }
    while (isspace((unsigned char)*src)) src++;
    size_t len = strlen(src);
    while (len > 0 && isspace((unsigned char)src[len - 1])) len--;
    strncpy(dst, src, len);
    dst[len] = '\0';
}
static void trim_trailing(char *src) {
    size_t length = strlen(src);
    while (length > 0 && isspace((unsigned char)src[length - 1])) length--;
    src[length] = '\0';
}

char* extract_value(char **line) {
    // If the input string is empty or we reached the end, return NULL
    if (*line == NULL || **line == '\0') {
        return NULL;
    }

    const char *current_start = *line;
    const char *next_comma = strchr(current_start, ',');
    int length;

    if (next_comma != NULL) {
        length = next_comma - current_start;
        // Advance the original pointer past the comma for the next call
        *line = (char *)(next_comma + 1);
    } else {
        length = strlen(current_start);
        // No more commas, advance original pointer to the very end
        *line = NULL;
    }

    // Allocate memory on the HEAP so it persists after returning
    char *destination = malloc(length + 1);
    char *destination2 = malloc(length + 1);
    if (destination == NULL) return NULL; // Always check if malloc succeeded

    strncpy(destination, current_start, length);
    destination[length] = '\0';
    trim_string(destination, destination2);
    return destination2;
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

    if (!feof(file) && buffer_result[strlen(buffer) -1] != '\n') {
        return bdb_error_set(err, BDB_ERR_PARSE,
                                 "line %" PRIu64 " exceeds the maximum length of %d bytes", *line_no, BDB_CSV_MAX_LINE);
    }

    trim_trailing(buffer);
    *dest = buffer;
    return BDB_OK;
}