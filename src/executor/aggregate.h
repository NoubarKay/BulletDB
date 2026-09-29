//
// Created by user on 9/29/2026.
//

#ifndef BULLETDB_AGGREGATE_H
#define BULLETDB_AGGREGATE_H
#include "bdb_operator.h"

typedef struct {
      BdbOperator base;
      const char *column_name;   // which column to sum, e.g. "PRICEEACH"
      uint64_t    column;        // its index, looked up in the first chunk
      bool        found_column;  // has the lookup been done yet?

      // running total (only one of the two sums is used, depending on the column type)
      enum ColumnType type;
      int64_t     int_sum;
      double      double_sum;

      bool        done;          // result already returned?
      CHUNK       result;        // 1 column × 1 row, returned by next
  } BdbAggregate;


void bdb_aggregate_init(BdbAggregate *agg, BdbOperator *child, const char *column_name);
#endif //BULLETDB_AGGREGATE_H
