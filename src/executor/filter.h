//
// Created by user on 10/1/2026.
//

#ifndef BULLETDB_FILTER_H
#define BULLETDB_FILTER_H
#include "bdb_operator.h"

typedef enum {
    BDB_OP_EQ,     // =
    BDB_OP_NE,     // !=
    BDB_OP_LT,     // <
    BDB_OP_LE,     // <=
    BDB_OP_GT,     // >
    BDB_OP_GE,     // >=
} BdbCompareOp;

typedef struct {
    BdbOperator base;

    const char* column_name;
    BdbCompareOp op;

    uint64_t column;        // its index, looked up in the first chunk
    bool found_column;  // has the lookup been done yet?

    const char* value;

    uint16_t sel[BDB_VECTOR_SIZE]; // internal filter buffer
    CHUNK out_chunk;
} BdbFilter;

void bdb_filter_init(BdbFilter *filter, BdbOperator *child, const char *column_name, BdbCompareOp type, const char* value);


#endif //BULLETDB_FILTER_H
