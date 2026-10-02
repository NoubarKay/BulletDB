//
// Created by user on 9/25/2026.
//

// Constants that define the .bdb file layout (see docs/DESIGN.md, Part 2).

#ifndef BULLETDB_BDB_FORMAT_H
#define BULLETDB_BDB_FORMAT_H

#include <stdbool.h>
#include <stdint.h>

#define BDB_MAX_COL_COUNT 100

#define BDB_MAGIC "BDB1"
#define BDB_MAGIC_SIZE 4
#define BDB_VERSION 2

typedef union {
    int64_t i;
    double  d;
} BdbStatValue;

typedef struct {
    bool         has_minmax;   // false when every value in the group is NULL
    BdbStatValue min;
    BdbStatValue max;
} BdbColumnStats;

#endif //BULLETDB_BDB_FORMAT_H
