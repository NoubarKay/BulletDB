//
// Created by user on 9/24/2026.
//

#ifndef BULLETDB_TABLE_H
#define BULLETDB_TABLE_H
#include <stddef.h>
#include <stdint.h>

#include "common.h"

// Stored in .bdb files as one byte, so these values must not change.
enum ColumnType {
    BDB_COL_INT = 0,   // int64_t
    BDB_COL_DOUBLE = 1,  // double
    BDB_COL_BOOL = 3,
    BDB_COL_STR = 4
};

typedef struct {
    char* name;
    enum ColumnType type;
    void* data;
    char* bitmap;
} COLUMN;

typedef struct {
    uint64_t row_count;
    uint64_t col_count;
    uint32_t group_size;
    COLUMN* columns;
} TABLE;

// Size in bytes of one value of the given type, or 0 if the type is unknown.
size_t bdb_col_type_size(enum ColumnType type);

void print_table(TABLE *table);
void free_table(TABLE *table);
BdbStatus bdb_find_column(const TABLE *table, const char *name, size_t *col_idx, BdbError *err);

#endif //BULLETDB_TABLE_H
