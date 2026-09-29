//
// Created by user on 9/29/2026.
//

#ifndef BULLETDB_BDB_OPERATOR_H
#define BULLETDB_BDB_OPERATOR_H
#include <stdio.h>

#include "common/common.h"
#include "core/chunk.h"
#include "storage/io.h"

typedef struct BdbOperator BdbOperator;

struct BdbOperator{
    BdbStatus (*next)(BdbOperator *self, const CHUNK **out, BdbError *err);
    void (*close)(BdbOperator *self);
    void (*describe)(BdbOperator *self, FILE *out);
    BdbOperator *child;
    uint64_t stat_calls;
    uint64_t stat_chunks;
    uint64_t stat_rows;
    uint64_t stat_time_ns;
};

static BdbStatus bdb_op_next(BdbOperator *op, const CHUNK **out, BdbError *err) {
    op->stat_calls++;
    uint64_t start = bdb_now();
    BdbStatus s = op->next(op, out, err);
    if (*out != NULL) {
        op->stat_chunks++;
        op->stat_rows += (*out)->count;
    }
    op->stat_time_ns += bdb_now() - start;
    return s;
}
#endif //BULLETDB_BDB_OPERATOR_H
