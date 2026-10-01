#include "csv/csv_sniffer.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/common.h"
#include "core/chunk.h"
#include "csv/csv_tokenize.h"

// Candidate types in promotion order: a column only ever moves to a larger
// value, so merging a value into a column is a max().
typedef enum { SNIFF_UNKNOWN = 0, SNIFF_BOOL, SNIFF_INT, SNIFF_DOUBLE, SNIFF_STR } SniffType;

typedef struct {
    SniffType type;         // SNIFF_UNKNOWN until the first non-empty value
    bool saw_bool_word;     // true/false/t/f/yes/no seen → can't become INT
} SniffState;

#define MAX_STATE(type, column) ((type) > (column) ? (type) : (column))

static SniffType kind_of(const char *field, bool *is_bool_word) {
    *is_bool_word = false;
    if (strcasecmp(field, "true") == 0 || strcasecmp(field, "false") == 0 ||
        strcasecmp(field, "t") == 0     || strcasecmp(field, "f") == 0     ||
        strcasecmp(field, "yes") == 0   || strcasecmp(field, "no") == 0) { *is_bool_word = true; return SNIFF_BOOL; }
    if (strcmp(field, "0") == 0 || strcmp(field, "1") == 0) return SNIFF_BOOL;

    char *end;
    errno = 0;
    strtoll(field, &end, 10);
    if (end != field && *end == '\0' && errno != ERANGE) return SNIFF_INT;

    strtod(field, &end);
    if (end != field && *end == '\0') return SNIFF_DOUBLE;

    return SNIFF_STR;
}

static void sniff_value(SniffState *s, const char *field) {
    bool is_bool_word;
    SniffType type = kind_of(field, &is_bool_word);
    if (is_bool_word) s->saw_bool_word = true;

    s->type = MAX_STATE(s->type, type);
    if (s->saw_bool_word && s->type >= SNIFF_INT)  s->type = SNIFF_STR;
}

BdbStatus csv_sniff(FILE *f, char *buffer, CHUNK *chunk, uint64_t *line_no, BdbError *err) {
    BdbStatus status;
    SniffState *states = calloc(chunk->col_count, sizeof(SniffState));

    if (states == NULL) {
        return bdb_error_set(err, BDB_ERR_NOMEM, "failed to allocate memory for sniff states");
    }

    int64_t rows = 0;
    char* dest = NULL;

    while (rows < BDB_CSV_SAMPLE_ROWS) {
        status = read_next_line(f, buffer, err, line_no, &dest);

        if (status != BDB_OK) {
            free(states);
            return status;
        }

        if (dest == NULL) {
            break;
        }

        if (dest[0] == '\0') continue;

        char *cursor = buffer;
        for (uint64_t i = 0; i < chunk->col_count; i++) {
            char *field = next_field(&cursor);

            if (field == NULL) {
                free(states);
                return bdb_error_set(err, BDB_ERR_PARSE, "line %llu: expected %llu values, got %llu",
                                     (unsigned long long)*line_no, chunk->col_count, i);
            }

            if (strlen(field) > BDB_CSV_MAX_FIELD) {
                free(states);
                return bdb_error_set(err, BDB_ERR_PARSE,
                                     "line %llu: value in column '%s' is longer than %d characters",
                                     (unsigned long long)*line_no, chunk->columns[i].name, BDB_CSV_MAX_FIELD);
            }

            if (field[0] == '\0') continue;

            sniff_value(&states[i], field);
        }

        if (next_field(&cursor) != NULL) {
            free(states);
            return bdb_error_set(err, BDB_ERR_PARSE,
                                 "line %llu: more values than the %llu columns in the header",
                                 (unsigned long long)*line_no, chunk->col_count);
        }
        rows++;
    }

    for (uint64_t i = 0; i < chunk->col_count; i++) {

        switch (states[i].type) {
          case SNIFF_BOOL:    chunk->columns[i].type = BDB_COL_BOOL;   break;
          case SNIFF_INT:     chunk->columns[i].type = BDB_COL_INT;    break;
          case SNIFF_UNKNOWN:                                          // all NULL in the sample
          case SNIFF_DOUBLE:  chunk->columns[i].type = BDB_COL_DOUBLE; break;
            case SNIFF_STR:     free(states); return bdb_error_set(err, BDB_ERR_PARSE, "column '%s' contains text; text columns aren't supported yet",
                       chunk->columns[i].name);
      }

    }

    free(states);

    return BDB_OK;
}
