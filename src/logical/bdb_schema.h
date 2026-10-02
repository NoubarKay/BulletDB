//
// Created by user on 10/2/2026.
//

#ifndef BULLETDB_BDB_SCHEMA_H
#define BULLETDB_BDB_SCHEMA_H

#include "logical/bdb_field.h"

typedef struct {
    BdbField *fields;
    uint16_t col_count;
} BdbSchema;

#endif //BULLETDB_BDB_SCHEMA_H
