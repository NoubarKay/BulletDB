#include "bdb_operator.h"
#include "executor/aggregate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static BdbStatus build_result(BdbAggregate *agg, BdbError *err) {
    if (agg->type != BDB_COL_INT && agg->type != BDB_COL_DOUBLE) {
        return bdb_error_set(err, BDB_ERR_INVALID,
                             "SUM needs an INT or DOUBLE column, '%s' is neither", agg->column_name);
    }

    CHUNK *result = &agg->result;

    result->columns = calloc(1, sizeof(COLUMN));
    if (result->columns == NULL) {
        return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory for aggregate result");
    }
    result->col_count = 1;   // set only once the array exists

    COLUMN *col = &result->columns[0];
    col->type = agg->type;

    size_t name_size = strlen("SUM()") + strlen(agg->column_name) + 1;   // +1 for '\0'
    col->name   = malloc(name_size);
    col->data   = calloc(1, bdb_col_type_size(col->type));               // room for 1 value
    col->bitmap = calloc(1, 1);                                          // 1 bitmap byte
    if (col->name == NULL || col->data == NULL || col->bitmap == NULL) {
        return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory for aggregate result");
    }
    snprintf(col->name, name_size, "SUM(%s)", agg->column_name);

    if (col->type == BDB_COL_INT) {
        ((int64_t *)col->data)[0] = agg->int_sum;
    } else {
        ((double *)col->data)[0] = agg->double_sum;
    }
    col->bitmap[0] = 1;
    result->count = 1;

    return BDB_OK;
}

static BdbStatus aggregate_next(BdbOperator *self, const CHUNK **out, BdbError *err) {

    BdbAggregate *agg = (BdbAggregate *)self;
    *out = NULL;

    while (agg->done) {
        return BDB_OK;
    }

    const CHUNK *chunk;
    BdbStatus status;

    while ((status = self->child->next(self->child, &chunk, err)) == BDB_OK && chunk != NULL) {
        if (agg->found_column == false) {
            for (int i = 0; i < chunk->col_count; i++) {
                if (strcmp(chunk->columns[i].name, agg->column_name) == 0) {
                    agg->found_column = true;
                    agg->type = chunk->columns[i].type;
                    agg->column = i;
                    break;
                }
            }

            if (agg->found_column == false) {
                return bdb_error_set(err, BDB_ERR_NOT_FOUND, "Column %s not found", agg->column_name);
            }
        }

        switch (agg->type) {
            case BDB_COL_INT:
                const int64_t *int_values = (const int64_t *)chunk->columns[agg->column].data;
                for (uint64_t i = 0; i < chunk->count; i++) {
                    if (chunk->columns[agg->column].bitmap[i]) agg->int_sum += int_values[i];
                }
                break;
            case BDB_COL_DOUBLE:
                const double *double_values = (const double *)chunk->columns[agg->column].data;
                for (uint64_t i = 0; i < chunk->count; i++) {
                    if (chunk->columns[agg->column].bitmap[i]) agg->double_sum += double_values[i];
                }
                break;
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

void bdb_aggregate_init(BdbAggregate *agg, BdbOperator *child, const char *column_name) {
    *agg = (BdbAggregate){0};

    agg->base.next = aggregate_next;
    agg->base.child = child;
    agg->column_name = column_name;
    agg->found_column = false;
    agg->done = false;
}