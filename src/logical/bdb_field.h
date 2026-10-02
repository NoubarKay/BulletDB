//
// Created by user on 10/2/2026.
//

#ifndef BULLETDB_BDBFIELD_H
#define BULLETDB_BDBFIELD_H
#include "core/table.h"

typedef struct {
    const char *name;
    enum ColumnType type;
} BdbField;

#endif //BULLETDB_BDBFIELD_H
