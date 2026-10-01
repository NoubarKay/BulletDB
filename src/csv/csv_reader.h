//
// Created by user on 9/24/2026.
//

#ifndef BULLETDB_CSV_H
#define BULLETDB_CSV_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "common/common.h"
#include "core/chunk.h"


typedef struct {
    FILE *file;
    char *buffer;
    uint64_t line_no;
    CHUNK chunk;
} CSV_READER;

BdbStatus csv_open(CSV_READER *reader, const char *path, BdbError *err);
BdbStatus csv_next_chunk(CSV_READER *r, const CHUNK **out, BdbError *err);
void csv_close(CSV_READER *reader);

#endif //BULLETDB_CSV_H
