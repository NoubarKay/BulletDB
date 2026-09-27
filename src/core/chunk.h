//
// Created by user on 9/27/2026.
//

#ifndef BULLETDB_CHUNK_H
#define BULLETDB_CHUNK_H
#include <stdint.h>

#include "core/table.h"

// Rows per chunk: the unit that moves through the engine.
#define BDB_VECTOR_SIZE     2048
// Rows per row group: the unit stored in .bdb files. Always a whole number
// of chunks, so a chunk never spans two row groups.
#define BDB_ROW_GROUP_SIZE  (60 * BDB_VECTOR_SIZE)

typedef struct {
    uint64_t count; //Number of rows in the chunk;
    uint64_t col_count;
    COLUMN *columns; //Array of columns in the chunk;
} CHUNK;

BdbStatus chunk_init(CHUNK *c, uint64_t col_count, BdbError *err);
void chunk_reset(CHUNK* c);
void chunk_free(CHUNK* c);

#endif //BULLETDB_CHUNK_H
