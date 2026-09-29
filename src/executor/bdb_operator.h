//
// Created by user on 9/29/2026.
//

#ifndef BULLETDB_BDB_OPERATOR_H
#define BULLETDB_BDB_OPERATOR_H
#include <stdio.h>

#include "common/common.h"
#include "core/chunk.h"

typedef struct BdbOperator BdbOperator;

struct BdbOperator{
    BdbStatus (*next)(BdbOperator *self, const CHUNK **out, BdbError *err);
    void (*close)(BdbOperator *self);
    void (*describe)(BdbOperator *self, FILE *out);
    BdbOperator *child;
};
#endif //BULLETDB_BDB_OPERATOR_H
