//
// Created by user on 10/1/2026.
//

#ifndef BULLETDB_CSV_SNIFFER_H
#define BULLETDB_CSV_SNIFFER_H

#define BDB_CSV_SAMPLE_ROWS 30720

#include <stdio.h>

#include "core/chunk.h"

typedef enum { SNIFF_UNKNOWN = 0, SNIFF_BOOL, SNIFF_INT, SNIFF_DOUBLE, SNIFF_STR } SniffType;

typedef struct {
    SniffType type;   // BDB_COL_UNKNOWN until the first non-empty value
    bool saw_bool_word;     // true/false/t/f/yes/no seen → can't become INT
} SniffState;

BdbStatus csv_sniff(FILE *f, char *buffer, CHUNK *chunk, uint64_t *line_no, BdbError *err);
void sniff_value(SniffState *s, const char *field);
#endif //BULLETDB_CSV_SNIFFER_H
