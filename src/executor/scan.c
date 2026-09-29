#include <inttypes.h>

#include "common/common.h"
#include "core/chunk.h"
#include "bdb_operator.h"
#include "scan.h"

static BdbStatus scan_next(BdbOperator *self, const CHUNK **out, BdbError *err) {
    BdbScan *scan = (BdbScan *)self;
    return bdb_reader_next_chunk(&scan->reader, out, err);
}

static void scan_close(BdbOperator *self) {
    BdbScan *scan = (BdbScan *)self;
    bdb_reader_close(&scan->reader);
}

static void scan_describe(BdbOperator *self, FILE *out) {
    BdbScan *scan = (BdbScan *)self;
    fprintf(out, "SCAN (%" PRIu64 " rows, %u columns, %u groups)\n",
            scan->reader.row_count, (unsigned)scan->reader.col_count, scan->reader.group_count);
    if (self->stat_calls > 0) {
        fprintf(out, "calls: %" PRIu64 "  chunks: %" PRIu64 "  rows: %" PRIu64
                "  time: %.3f ms"
                "  (times next() was invoked | non-empty chunks returned | total rows emitted)\n",
                self->stat_calls, self->stat_chunks, self->stat_rows,
                (double)self->stat_time_ns / 1e6);
    }
}

BdbStatus bdb_scan_open(BdbScan *scan, const char *path, BdbError *err) {
    *scan = (BdbScan){0};
    scan->base.next = scan_next;
    scan->base.close = scan_close;
    scan->base.describe = scan_describe;
    return bdb_reader_open(&scan->reader, path, err);
}
