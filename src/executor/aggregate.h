//
// Created by user on 9/29/2026.
//

#ifndef BULLETDB_AGGREGATE_H
#define BULLETDB_AGGREGATE_H
#include "bdb_operator.h"

typedef enum {
    BDB_AGG_SUM,
    BDB_AGG_COUNT,
    BDB_AGG_MIN,
    BDB_AGG_MAX,
    BDB_AGG_AVG
} BdbAggregateType;

typedef struct {
    BdbOperator base;
    const char *column_name;   // which column to sum, e.g. "PRICEEACH"
    uint64_t    column;        // its index, looked up in the first chunk
    bool        found_column;  // has the lookup been done yet?
    BdbAggregateType type;

    // running total (only one of the two sums is used, depending on the column type)
    enum ColumnType col_type;
    int64_t     int_sum;
    double      double_sum;

    uint64_t   count;       // non-NULL values seen (COUNT, AVG, and NULL results)
    bool       has_value;   // for MIN/MAX: has the first value been seen?
    int64_t    int_min,    int_max;
    double     double_min, double_max;

    bool        done;          // result already returned?
    CHUNK       result;        // 1 column × 1 row, returned by next
} BdbAggregate;


void bdb_aggregate_init(BdbAggregate *agg, BdbOperator *child, BdbAggregateType type, const char *column_name);
#endif //BULLETDB_AGGREGATE_H
