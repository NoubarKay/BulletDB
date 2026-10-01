

#include "bdb_operator.h"
#include "filter.h"

#include <inttypes.h>
#include <stdlib.h>     // strtod
#include <string.h>     // strcmp

static const char *OP_NAMES[] = { "=", "!=", "<", "<=", ">", ">=" };


#define FILTER_LOOP(T, CMP)                                                     \
    do {                                                                        \
        const T *v = col->data;                                                 \
        const T  x = (T)value;                                                  \
        if (chunk->sel_vector == NULL) {          /* flat input: k == i */      \
            for (uint64_t k = 0; k < chunk->count; k++) {                       \
                filter->sel[matches] = (uint16_t)k;                             \
                matches += (col->bitmap[k] != 0) & (v[k] CMP x);                \
            }                                                                   \
        } else {                                  /* input already selected */  \
            for (uint64_t i = 0; i < chunk->count; i++) {                       \
                uint64_t k = chunk->sel_vector[i];                              \
                filter->sel[matches] = (uint16_t)k;                             \
                matches += (col->bitmap[k] != 0) & (v[k] CMP x);                \
            }                                                                   \
        }                                                                       \
    } while (0)

// Picks the loop for the filter's comparison, once per chunk.
#define FILTER_OPS(T)                                                           \
    switch (filter->op) {                                                       \
        case BDB_COMPARE_EQ: FILTER_LOOP(T, ==); break;                         \
        case BDB_COMPARE_NE: FILTER_LOOP(T, !=); break;                         \
        case BDB_COMPARE_LT: FILTER_LOOP(T, < ); break;                         \
        case BDB_COMPARE_LE: FILTER_LOOP(T, <=); break;                         \
        case BDB_COMPARE_GT: FILTER_LOOP(T, > ); break;                         \
        case BDB_COMPARE_GE: FILTER_LOOP(T, >=); break;                         \
    }

static void filter_close(BdbOperator *self) {
    if (self->child != NULL) {
        self->child->close(self->child);   // closes the scan below
    }
}

static void filter_describe(BdbOperator *self, FILE *out) {
    BdbFilter *f = (BdbFilter *)self;
    fprintf(out, "FILTER %s %s %s ", f->column_name, OP_NAMES[f->op], f->value);
    if (self->stat_calls > 0) {
        uint64_t child_ns = self->child ? self->child->stat_time_ns : 0;
        uint64_t self_ns  = self->stat_time_ns > child_ns ? self->stat_time_ns - child_ns : 0;
        fprintf(out, "calls: %" PRIu64 "  chunks: %" PRIu64 "  rows: %" PRIu64
                "  time: %.3f ms"
                "  (times next() was invoked | non-empty chunks returned | total rows emitted | self time excl. child)\n",
                self->stat_calls, self->stat_chunks, self->stat_rows,
                (double)self_ns / 1e6);
    }
}

static BdbStatus filter_next(BdbOperator *self, const CHUNK **out, BdbError *err) {
    BdbFilter *filter = (BdbFilter *)self;
    *out = NULL;

    const CHUNK *chunk;
    BdbStatus status;

    while ((status = bdb_op_next(self->child, &chunk, err)) == BDB_OK && chunk != NULL) {

        //LOOKUP COL TO FILTER ON
        if (filter->found_column == false) {
            for (int i = 0; i < chunk->col_count; i++) {
                if (strcmp(chunk->columns[i].name, filter->column_name) == 0) {
                    filter->found_column = true;
                    filter->col_type = chunk->columns[i].type;
                    filter->column = i;
                    break;
                }
            }

            if (filter->found_column == false) {
                return bdb_error_set(err, BDB_ERR_NOT_FOUND, "Column %s not found", filter->column_name);
            }
        }

        const COLUMN *col = &chunk->columns[filter->column];
        uint16_t matches = 0;

        double value = filter->number;
        switch (filter->col_type) {
            case BDB_COL_INT:
                FILTER_OPS(int64_t);
                break;
            case BDB_COL_DOUBLE:
                FILTER_OPS(double);
                break;
            default:
                return bdb_error_set(err, BDB_ERR_INVALID,
                                     "can't filter on column '%s': only INT and DOUBLE are supported",
                                     filter->column_name);
        }

        if (matches > 0) {
            filter->out_chunk.columns    = chunk->columns;
            filter->out_chunk.col_count  = chunk->col_count;
            filter->out_chunk.count      = matches;
            filter->out_chunk.sel_vector = filter->sel;
            *out = &filter->out_chunk;
            return BDB_OK;
        }

        filter->out_chunk.count = 0;
        filter->out_chunk.col_count = chunk->col_count;
        filter->out_chunk.columns = chunk->columns;
        filter->out_chunk.sel_vector = NULL;
        *out = &filter->out_chunk;
        return BDB_OK;
    }
    return status;
}

void bdb_filter_init(BdbFilter *filter, BdbOperator *child, const char *column_name, BdbCompareOp type, const char* value) {
    *filter = (BdbFilter){0};

    filter->base.next = filter_next;
    filter->base.child = child;
    filter->base.close = filter_close;
    filter->base.describe = filter_describe;
    filter->column_name = column_name;
    filter->op = type;
    filter->value = value;
    filter->number = strtod(value, NULL);
}
