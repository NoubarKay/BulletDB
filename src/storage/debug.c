//
// Created by user on 9/29/2026.
//

#include "storage/debug.h"

#include <inttypes.h>
#include <stdbool.h>

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

static const char *type_name(enum ColumnType type) {
    switch (type) {
        case BDB_COL_INT:    return "INT";
        case BDB_COL_DOUBLE: return "DOUBLE";
        case BDB_COL_BOOL:   return "BOOL";
        default:             return "UNKNOWN";
    }
}

int bdb_reader_debug_dump(const BDB_READER *r, FILE *out) {
    int problems = 0;
    const CHUNK *schema = &r->chunk;

    fprintf(out, "== BDB_READER ==\n");
    fprintf(out, "rows: %" PRIu64 "   columns: %u   groups: %" PRIu32 "\n",
            r->row_count, (unsigned)r->col_count, r->group_count);

    // --- schema ---
    if (r->col_count == 0 || r->col_count > BDB_MAX_COL_COUNT) {
        fprintf(out, "  !! col_count %u is outside 1..%d\n", (unsigned)r->col_count, BDB_MAX_COL_COUNT);
        problems++;
    }
    if (schema->col_count != r->col_count) {
        fprintf(out, "  !! chunk.col_count %" PRIu64 " != footer col_count %u\n",
                schema->col_count, (unsigned)r->col_count);
        problems++;
    }
    if (schema->columns == NULL) {
        fprintf(out, "  !! no columns loaded (chunk.columns is NULL)\n");
        problems++;
    } else {
        for (uint64_t c = 0; c < schema->col_count; c++) {
            const COLUMN *col = &schema->columns[c];
            fprintf(out, "  col %" PRIu64 ": %-20s %-7s (type %d)\n",
                    c, col->name ? col->name : "(null)", type_name(col->type), (int)col->type);
            if (col->name == NULL) {
                fprintf(out, "  !! column %" PRIu64 " has no name\n", c);
                problems++;
            }
            if (bdb_col_type_size(col->type) == 0) {
                fprintf(out, "  !! column %" PRIu64 " has unknown type %d\n", c, (int)col->type);
                problems++;
            }
        }
    }

    // --- groups ---
    if (r->group_count > 0 && r->row_groups == NULL) {
        fprintf(out, "  !! %" PRIu32 " groups but no row counts loaded\n", r->group_count);
        return problems + 1;
    }

    // Blocks are written back to back, starting right after the 8-byte header.
    uint64_t expected = BDB_MAGIC_SIZE + sizeof(uint32_t);
    uint64_t total_rows = 0;
    bool have_offsets = r->offsets != NULL;

    if (r->group_count > 0 && !have_offsets) {
        fprintf(out, "  (offsets not loaded yet: skipping offset checks)\n");
    }

    for (uint32_t g = 0; g < r->group_count; g++) {
        uint32_t rows = r->row_groups[g];
        total_rows += rows;
        fprintf(out, "  group %" PRIu32 ": %" PRIu32 " rows\n", g, rows);

        if (rows == 0 || rows > BDB_ROW_GROUP_SIZE) {
            fprintf(out, "  !! group %" PRIu32 " has %" PRIu32 " rows, expected 1..%d\n",
                    g, rows, BDB_ROW_GROUP_SIZE);
            problems++;
        }
        if (!have_offsets || schema->columns == NULL) continue;

        for (uint64_t c = 0; c < schema->col_count; c++) {
            uint64_t offset = r->offsets[(size_t)g * r->col_count + c];
            uint64_t size   = (uint64_t)rows * (1 + bdb_col_type_size(schema->columns[c].type));
            bool ok = offset == expected;

            fprintf(out, "    %-20s offset %10" PRIu64 " (0x%08" PRIX64 ")  size %10" PRIu64 "  %s\n",
                    schema->columns[c].name ? schema->columns[c].name : "(null)",
                    offset, offset, size, ok ? "ok" : "MISMATCH");
            if (!ok) {
                fprintf(out, "  !! group %" PRIu32 " column %" PRIu64 ": offset %" PRIu64
                             ", expected %" PRIu64 "\n", g, c, offset, expected);
                problems++;
            }
            expected = offset + size;
        }
    }

    if (total_rows != r->row_count) {
        fprintf(out, "  !! group rows add up to %" PRIu64 ", footer row_count is %" PRIu64 "\n",
                total_rows, r->row_count);
        problems++;
    }
    if (have_offsets) {
        fprintf(out, "  data ends at %" PRIu64 " (should equal the trailer's footer_offset)\n", expected);
    }

    fprintf(out, "== %d problem%s ==\n", problems, problems == 1 ? "" : "s");
    return problems;
}
