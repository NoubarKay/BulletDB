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
} BDB_READER;

BdbStatus bdb_reader_open(BDB_READER *reader, const char *path, BdbError *err);


#endif //BULLETDB_BDB_READER_H
