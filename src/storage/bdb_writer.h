//
// Created by user on 9/28/2026.
//

#ifndef BULLETDB_BDB_WRITER_H
#define BULLETDB_BDB_WRITER_H
#include <stdio.h>

#include "core/chunk.h"
#include "common/common.h"



typedef struct {
    FILE* file;
    CHUNK group;
    bool have_schema;

    uint64_t  row_count;      // total rows written
    uint16_t  col_count;      // total rows written
    uint32_t  group_count;
    uint32_t *group_rows;     // [group_count]            rows in each group
    uint64_t *offsets;        // [group_count * col_count] block offset of each column
    uint32_t  groups_capacity; // allocated length of group_rows/offsets (grow by doubling)
} BDB_WRITER;

BdbStatus bdb_writer_open(BDB_WRITER* writer, const char* path, BdbError* err);
BdbStatus bdb_writer_append(BDB_WRITER *w, const CHUNK *chunk, BdbError *err);
void bdb_writer_close(BDB_WRITER* writer);
BdbStatus bdb_writer_finish(BDB_WRITER *w, BdbError *err);
#endif //BULLETDB_BDB_WRITER_H
