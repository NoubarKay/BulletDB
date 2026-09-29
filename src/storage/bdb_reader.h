//
// Created by user on 9/29/2026.
//

#ifndef BULLETDB_BDB_READER_H
#define BULLETDB_BDB_READER_H
#include <stdint.h>
#include <stdio.h>

#include "common/common.h"
#include "core/chunk.h"


typedef struct {
    FILE *file;
    uint64_t  row_count;
    uint16_t  col_count;
    uint32_t  group_count;

    //will contain schema (names, types) + buffers for BDB_VECTOR_SIZE rows, will be reused
    CHUNK chunk;

    uint32_t *row_groups;
    uint64_t *offsets;

    //current row group we are on
    uint32_t group_index;

    //current rows in the current row group we are on
    uint64_t rows_in_group;
} BDB_READER;

BdbStatus bdb_reader_open(BDB_READER *reader, const char *path, BdbError *err);

BdbStatus bdb_reader_next_chunk(BDB_READER *reader, const CHUNK **out, BdbError *err);

void bdb_reader_close(BDB_READER *reader);


#endif //BULLETDB_BDB_READER_H
