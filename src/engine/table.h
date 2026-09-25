//
// Created by user on 9/24/2026.
//

#ifndef BULLETDB_TABLE_H
#define BULLETDB_TABLE_H
#include <stdint.h>

#include "common.h"

typedef struct {
    char* name;
    int64_t* data;
} COLUMN;

typedef struct {
    uint64_t row_count;
    uint64_t col_count;
    uint32_t group_size;
    COLUMN* columns;
} TABLE;

void print_table(TABLE *table);
void free_table(TABLE *table);
BdbStatus bdb_find_column(const TABLE *table, const char *name, size_t *col_idx, BdbError *err);

#endif //BULLETDB_TABLE_H
