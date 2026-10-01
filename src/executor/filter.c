

#include "bdb_operator.h"
#include "filter.h"

#include <inttypes.h>

static const char *OP_NAMES[] = { "=", "!=", "<", "<=", ">", ">=" };

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
    return bdb_op_next(self->child, out, err);     // TEMPORARY: passes everything through
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
}
