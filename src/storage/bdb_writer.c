
#include "common/common.h"
#include "bdb_writer.h"

#include <stdlib.h>
#include <string.h>

#include "bdb_format.h"
#include "memory.h"

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
    CHUNK *g = &w->group;
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

static BdbStatus bdb_write_group(BDB_WRITER *w, BdbError *err) {
      if (w->group_count == w->groups_capacity) {                        // full → grow
          uint32_t old_cap = w->groups_capacity;
          uint32_t new_cap = GROW_CAPACITY(old_cap);

          uint32_t *rows = GROW_ARRAY(uint32_t, w->group_rows, old_cap, new_cap);
          if (rows == NULL) return bdb_error_set(err, BDB_ERR_NOMEM, "could not grow group list");
          w->group_rows = rows;

          uint64_t *offs = GROW_ARRAY(uint64_t, w->offsets,
                                      (size_t)old_cap * w->col_count,
                                      (size_t)new_cap * w->col_count);    // × col_count
          if (offs == NULL) return bdb_error_set(err, BDB_ERR_NOMEM, "could not grow offsets");
          w->offsets = offs;

          w->groups_capacity = new_cap;
      }

    for (uint64_t i=0; i < w->group.col_count; i++) {
        size_t offset = _ftelli64(w->file);
        w->offsets[w->group_count * w->col_count + i] = offset;
        size_t col_type_size = bdb_col_type_size(w->group.columns[i].type);

        size_t written = fwrite(w->group.columns[i].bitmap, 1, w->group.count, w->file);
        if (written != w->group.count) {
            return bdb_error_set(err, BDB_ERR_IO, "failed to write bitmap for column '%s'", w->group.columns[i].name);
        }
        written = fwrite(w->group.columns[i].data, col_type_size, w->group.count, w->file);
        if (written != w->group.count) {
            return bdb_error_set(err, BDB_ERR_IO, "failed to write data for column '%s'", w->group.columns[i].name);
        }
    }
    w->row_count+=w->group.count;
    w->group_rows[w->group_count] = w->group.count;
    w->group_count++;

    chunk_reset(&w->group);
    return BDB_OK;
}

BdbStatus bdb_writer_append(BDB_WRITER *w, const CHUNK *chunk, BdbError *err) {
    CHUNK *g = &w->group;
    BdbStatus status = BDB_OK;

    if (!w->have_schema) {
        status = init_group(w, chunk, err);
        if (status != BDB_OK)
            return status;

        w->col_count = g->col_count;
    }

    //if no more space in group
    if (w->group.count + chunk-> count > BDB_ROW_GROUP_SIZE) {
        return bdb_error_set(err, BDB_ERR_INVALID, "chunk size exceeds row group size");
    }

    for (uint64_t c = 0; c < g->col_count; c++) {
        size_t col_type_size = bdb_col_type_size(g->columns[c].type);
        memcpy(g->columns[c].data + w->group.count * col_type_size, chunk->columns[c].data, chunk->count * col_type_size);
        memcpy(g->columns[c].bitmap + g->count,
               chunk->columns[c].bitmap, chunk->count);
    }

    g->count += chunk->count;

    if (g->count == BDB_ROW_GROUP_SIZE) {
        return bdb_write_group(w, err);
    }
    return BDB_OK;
}

static BdbStatus bdb_write_footer(BDB_WRITER *w, BdbError *err) {
    fwrite(&w->row_count, sizeof(uint64_t), 1, w->file);
    fwrite(&w->col_count, sizeof(uint16_t), 1, w->file);

    for (int i = 0; i < w->col_count; i++) {
        int16_t length = strlen(w->group.columns[i].name);
        fwrite(&length, sizeof(uint16_t), 1, w->file);
        fwrite(w->group.columns[i].name, length, 1, w->file);
        fwrite(&w->group.columns[i].type, sizeof(uint8_t), 1, w->file);
    }

    fwrite(&w->group_count, sizeof(uint32_t), 1, w->file);
}

BdbStatus bdb_writer_finish(BDB_WRITER *w, BdbError *err) {
    if (w->group.count > 0)
        bdb_write_group(w, err);


    uint64_t footer_offset = (uint64_t)ftell(w->file);
    bdb_write_footer(w, err);

    fwrite(&footer_offset, sizeof(uint64_t), 1, w->file);
    fwrite(BDB_MAGIC, BDB_MAGIC_SIZE, 1, w->file);

    if (fflush(w->file) != 0) {
        return bdb_error_set(err, BDB_ERR_IO, "could not flush file");
    }
}
