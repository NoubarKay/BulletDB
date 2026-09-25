//
// Created by user on 9/25/2026.
//

#ifndef BULLETDB_QUERY_H
#define BULLETDB_QUERY_H
#include <stdint.h>
#include "../common.h"
#include "../table.h"

enum BdbAggregate {
    BDB_AGG_SUM,
    BDB_AGG_COUNT,
};

enum BdbOperator {
    BDB_OP_EQ,
    BDB_OP_NE,
    BDB_OP_LT,
    BDB_OP_LE,
    BDB_OP_GT,
    BDB_OP_GE,
};

struct BdbFilter {
    const char* field;
    enum BdbOperator op;
    int64_t value;
};

typedef struct BdbQuery {
    enum BdbAggregate agg;
    const char* field;
    bool has_filter;
    struct BdbFilter filter;
} BdbQuery;

BdbStatus bdb_run_query(const TABLE* table, const BdbQuery* query, int64_t* result, BdbError *err);

#endif //BULLETDB_QUERY_H
