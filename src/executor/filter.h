//
// Created by user on 10/1/2026.
//

#ifndef BULLETDB_FILTER_H
#define BULLETDB_FILTER_H
#include "bdb_operator.h"

typedef enum {
    BDB_COMPARE_EQ,     // =
    BDB_COMPARE_NE,     // !=
    BDB_COMPARE_LT,     // <
    BDB_COMPARE_LE,     // <=
    BDB_COMPARE_GT,     // >
    BDB_COMPARE_GE,     // >=
} BdbCompareOp;

typedef struct {
    BdbOperator base;

    const char* column_name;
    BdbCompareOp op;

    uint64_t column;        // its index, looked up in the first chunk
    bool found_column;  // has the lookup been done yet?
    enum ColumnType col_type;

    const char* value;      // the constant as given, e.g. "22" (kept for describe)
    double      number;     // the same constant, parsed once in bdb_filter_init

    uint16_t sel[BDB_VECTOR_SIZE]; // internal filter buffer
    CHUNK out_chunk;
} BdbFilter;

void bdb_filter_init(BdbFilter *filter, BdbOperator *child, const char *column_name, BdbCompareOp type, const char* value);


#endif //BULLETDB_FILTER_H
