#include <stdint.h>
#include "query.h"
#include "../common.h"
#include "../table.h"

//
// Created by user on 9/25/2026.
//


static bool bdb_query_compare(const enum BdbOperator op, int64_t value_1, int64_t value_2) {
    switch (op) {
        case BDB_OP_EQ:
            return value_1 == value_2;
            break;
        case BDB_OP_NE:
            return value_1 != value_2;
            break;
        case BDB_OP_LT:
            return value_1 < value_2;
            break;;
        case BDB_OP_LE:
            return value_1 <= value_2;
            break;
        case BDB_OP_GT:
            return value_1 > value_2;
            break;
        case BDB_OP_GE:
            return value_1 >= value_2;
            break;
        default:
            return false;
            break;
    }
}


BdbStatus bdb_run_query(const TABLE* table, const BdbQuery* query, int64_t* result, BdbError *err) {
    size_t col_idx = 0;
    size_t filter_col_idx = 0;
    int64_t sum = 0;

    BdbStatus status = BDB_OK;

    status = bdb_find_column(table, query->field, &col_idx, err);

    if (status != BDB_OK) {
        return status;
    }

    if (query->has_filter) {
        status = bdb_find_column(table, query->filter.field, &filter_col_idx, err);
    }

    if (status != BDB_OK) {
        return status;
    }

    for (size_t i = 0; i < table->row_count; i++) {
        if (query->has_filter) {
            int64_t value = table->columns[filter_col_idx].data[i];

            if (!bdb_query_compare(query->filter.op, value, query->filter.value)) {
                continue;
            }
        }
        sum+= table->columns[col_idx].data[i];
    }

    *result = sum;

    return BDB_OK;
}

