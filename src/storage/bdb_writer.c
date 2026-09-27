
#include "common/common.h"
#include "bdb_writer.h"

#include <stdlib.h>
#include <string.h>

#include "bdb_format.h"

BdbStatus bdb_writer_open(BDB_WRITER *writer, const char *path, BdbError *err) {
    *writer = (BDB_WRITER){0};

    writer->file = fopen(path, "wb");
    if (writer->file == NULL) {
        return bdb_error_set(err, BDB_ERR_OPEN, "could not open '%s'", path);
    }

    uint32_t version = BDB_VERSION;
    if (fwrite(BDB_MAGIC, 1, BDB_MAGIC_SIZE, writer->file) != BDB_MAGIC_SIZE ||
        fwrite(&version, sizeof(version), 1, writer->file) != 1) {
        fclose(writer->file);
        writer->file = NULL;
        return bdb_error_set(err, BDB_ERR_IO, "could not write header to '%s'", path);
        }
    return BDB_OK;
}

void bdb_writer_close(BDB_WRITER *writer) {
    chunk_free(&writer->group);

    if (writer->file != NULL) {
        fclose(writer->file);
        writer->file = NULL;
    }
    writer->have_schema = false;
}

static BdbStatus init_group(BDB_WRITER *w, const CHUNK *chunk, BdbError *err) {
    CHUNK *g = w->group;
    g->columns = calloc(chunk->col_count, sizeof(COLUMN));
    if (g->columns == NULL) {
        return bdb_error_set(err, BDB_ERR_NOMEM, "could not allocate group columns");
    }
    g->col_count = chunk->col_count;
    for (uint64_t c = 0; c < g->col_count; c++) {
        COLUMN *dst = &g->columns[c];
        const COLUMN *src = &chunk->columns[c];
        dst->type   = src->type;
        dst->name   = strdup(src->name);
        dst->data   = calloc(BDB_ROW_GROUP_SIZE, bdb_col_type_size(src->type));
        dst->bitmap = calloc(BDB_ROW_GROUP_SIZE, 1);
        if (dst->name == NULL || dst->data == NULL || dst->bitmap == NULL) {
            return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory for column '%s'", src->name);
        }
    }
    w->have_schema = true;


    return BDB_OK;
}