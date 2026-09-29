#include "bdb_operator.h"
#include "executor/aggregate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const char *AGG_NAMES[] = { "SUM", "COUNT", "MIN", "MAX", "AVG" };

static BdbStatus build_result(BdbAggregate *agg, BdbError *err) {
    enum ColumnType return_type;

    if (agg->type == BDB_AGG_COUNT) {
        return_type = BDB_COL_INT;           // a count is always a whole number
    } else if (agg->type == BDB_AGG_AVG) {
        return_type = BDB_COL_DOUBLE;        // the average of 1 and 2 is 1.5
    } else {
        return_type = agg->col_type;         // SUM, MIN, MAX: same type as the column
    }

    CHUNK *result = &agg->result;

    result->columns = calloc(1, sizeof(COLUMN));
    if (result->columns == NULL) {
        return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory for aggregate result");
    }
    result->col_count = 1;   // set only once the array exists

    COLUMN *col = &result->columns[0];
    col->type = return_type;

    // "NAME(column)": the two names, plus 3 for '(', ')' and '\0'
    size_t name_size = strlen(AGG_NAMES[agg->type]) + strlen(agg->column_name) + 3;
    col->name   = malloc(name_size);
    col->data   = calloc(1, bdb_col_type_size(return_type));               // room for 1 value
    col->bitmap = calloc(1, 1);                                          // 1 bitmap byte
    if (col->name == NULL || col->data == NULL || col->bitmap == NULL) {
        return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory for aggregate result");
    }


    snprintf(col->name, name_size, "%s(%s)", AGG_NAMES[agg->type], agg->column_name);
    result->count = 1;

    // No non-NULL values seen: COUNT is 0, every other aggregate is NULL (as in SQL).
    if (agg->count == 0 && agg->type != BDB_AGG_COUNT) {
        col->bitmap[0] = 0;
        return BDB_OK;
    }

    // Write the one value. C can't pick a pointer type from a runtime value,
    // so each case casts data to the result type it knows it has.
    bool is_int = agg->col_type == BDB_COL_INT;

    switch (agg->type) {
        case BDB_AGG_COUNT:
            ((int64_t *)col->data)[0] = (int64_t)agg->count;
            break;
        case BDB_AGG_SUM:
            if (is_int) ((int64_t *)col->data)[0] = agg->int_sum;
            else        ((double  *)col->data)[0] = agg->double_sum;
            break;
        case BDB_AGG_MIN:
            if (is_int) ((int64_t *)col->data)[0] = agg->int_min;
            else        ((double  *)col->data)[0] = agg->double_min;
            break;
        case BDB_AGG_MAX:
            if (is_int) ((int64_t *)col->data)[0] = agg->int_max;
            else        ((double  *)col->data)[0] = agg->double_max;
            break;
        case BDB_AGG_AVG: {
            double total = is_int ? (double)agg->int_sum : agg->double_sum;
            ((double *)col->data)[0] = total / (double)agg->count;
            break;
        }
    }
    col->bitmap[0] = 1;

    return BDB_OK;
}

static BdbStatus aggregate_next(BdbOperator *self, const CHUNK **out, BdbError *err) {

    BdbAggregate *agg = (BdbAggregate *)self;
    *out = NULL;

    if (agg->done) {
        return BDB_OK;
    }

    const CHUNK *chunk;
    BdbStatus status;

    while ((status = self->child->next(self->child, &chunk, err)) == BDB_OK && chunk != NULL) {
        if (agg->found_column == false) {
            for (int i = 0; i < chunk->col_count; i++) {
                if (strcmp(chunk->columns[i].name, agg->column_name) == 0) {
                    agg->found_column = true;
                    agg->col_type = chunk->columns[i].type;
                    agg->column = i;
                    break;
                }
            }

            if (agg->found_column == false) {
                return bdb_error_set(err, BDB_ERR_NOT_FOUND, "Column %s not found", agg->column_name);
            }
        }

        const COLUMN *col = &chunk->columns[agg->column];
        switch (agg->col_type) {
            case BDB_COL_INT: {
                const int64_t *v = (const int64_t *)col->data;
                for (uint64_t i = 0; i < chunk->count; i++) {
                    if (!col->bitmap[i]) continue;              // skip NULLs
                    agg->count++;
                    agg->int_sum += v[i];
                    if (!agg->has_value) { agg->int_min = agg->int_max = v[i]; agg->has_value = true; }
                    else { if (v[i] < agg->int_min) agg->int_min = v[i];
                        if (v[i] > agg->int_max) agg->int_max = v[i]; }
                }
                break;
            }
            case BDB_COL_DOUBLE: {
                const double *v = (const double *)col->data;
                for (uint64_t i = 0; i < chunk->count; i++) {
                    if (!col->bitmap[i]) continue;              // skip NULLs
                    agg->count++;
                    agg->double_sum += v[i];
                    if (!agg->has_value) { agg->double_min = agg->double_max = v[i]; agg->has_value = true; }
                    else { if (v[i] < agg->double_min) agg->double_min = v[i];
                        if (v[i] > agg->double_max) agg->double_max = v[i]; }
                }
                break;
            }
            default:
                return bdb_error_set(err, BDB_ERR_INVALID, "Invalid column type %d for aggregate %d", (int)col->type, AGG_NAMES[agg->type]);
        }
    }

    if (status != BDB_OK) {
        return status;
    }

    status = build_result(agg, err);
    if (status != BDB_OK) {
        return status;
    }

    agg->done = true;
    *out = &agg->result;
    return BDB_OK;
}

static void aggregate_describe(BdbOperator *self, FILE *out) {
    BdbAggregate *agg = (BdbAggregate *)self;
    fprintf(out, "AGGREGATE %s(%s)\n", AGG_NAMES[agg->type], agg->column_name);
}

static void aggregate_close(BdbOperator *self) {
  BdbAggregate *agg = (BdbAggregate *)self;
  if (self->child != NULL) {
      self->child->close(self->child);    // closes the scan and its reader
  }
  chunk_free(&agg->result);
}

void bdb_aggregate_init(BdbAggregate *agg, BdbOperator *child, BdbAggregateType type, const char *column_name) {
    *agg = (BdbAggregate){0};

    agg->base.next = aggregate_next;
    agg->base.child = child;
    agg->base.describe = aggregate_describe;
    agg->base.close = aggregate_close;
    agg->column_name = column_name;
    agg->found_column = false;
    agg->done = false;
    agg->type = type;
}