//
// Created by user on 9/29/2026.
//

#include "storage/debug.h"

#include <inttypes.h>

#include "storage/bdb_format.h"

void bdb_writer_debug_dump(const BDB_WRITER *w, FILE *out) {
    const CHUNK *g = &w->group;

    fprintf(out, "== BDB_WRITER ==\n");
    fprintf(out, "rows written: %" PRIu64 "   columns: %" PRIu64
                 "   groups: %" PRIu32 " (capacity %" PRIu32 ")   rows in buffer: %" PRIu64 "\n",
            w->row_count, g->col_count, w->group_count, w->groups_capacity, g->count);

    for (uint64_t c = 0; c < g->col_count; c++) {
        fprintf(out, "  col %" PRIu64 ": %-20s type %d, %zu bytes per value\n",
                c, g->columns[c].name, (int)g->columns[c].type,
                bdb_col_type_size(g->columns[c].type));
    }

    if (w->offsets == NULL || w->group_rows == NULL) {
        fprintf(out, "  (no offsets recorded yet)\n");
        return;
    }

    uint64_t expected = BDB_MAGIC_SIZE + sizeof(uint32_t);

    for (uint32_t grp = 0; grp < w->group_count; grp++) {
        uint32_t rows = w->group_rows[grp];
        fprintf(out, "  group %" PRIu32 ": %" PRIu32 " rows\n", grp, rows);

        for (uint64_t c = 0; c < g->col_count; c++) {
            uint64_t offset = w->offsets[grp * g->col_count + c];
            uint64_t size   = (uint64_t)rows * (1 + bdb_col_type_size(g->columns[c].type));

            fprintf(out, "    %-20s offset %10" PRIu64 " (0x%08" PRIX64 ")  size %10" PRIu64 "  %s\n",
                    g->columns[c].name, offset, offset, size,
                    offset == expected ? "ok" : "MISMATCH");
            if (offset != expected) {
                fprintf(out, "      expected offset %" PRIu64 "\n", expected);
            }
            expected = offset + size;
        }
    }
    fprintf(out, "  data ends at %" PRIu64 " (footer should start here)\n", expected);
}
