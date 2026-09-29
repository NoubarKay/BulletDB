#include <stdio.h>

#include "common/common.h"
#include "bdb_reader.h"
#include "io.h"

#include <stdlib.h>
#include <string.h>

#include "bdb_format.h"

BdbStatus bdb_reader_open(BDB_READER *reader, const char *path, BdbError *err) {
    *reader = (BDB_READER){0};

    reader->file = fopen(path, "rb");
    if (reader->file == NULL) {
        return bdb_error_set(err, BDB_ERR_OPEN, "could not open '%s'", path);
    }

    int64_t file_size;
    if (bdb_fseek(reader->file, 0, SEEK_END) != 0) {
        return bdb_error_set(err, BDB_ERR_IO, "could not seek to end of file '%s'", path);
    }
    file_size = bdb_ftell(reader->file);
    if (file_size < 20) {
        return bdb_error_set(err, BDB_ERR_FORMAT, "Invalid or corrupted file '%s'", path);
    }
    rewind(reader->file);

    char magic[BDB_MAGIC_SIZE];
    uint32_t version;
    uint64_t footer_offset;

    //check header
    if (fread(magic, 1, BDB_MAGIC_SIZE, reader->file) != BDB_MAGIC_SIZE || fread(&version, sizeof(version), 1, reader->file) != 1) {
        return bdb_error_set(err, BDB_ERR_FORMAT, "Invalid or corrupted file '%s': Header magic not found.", path);
    }

    if (memcmp(magic, BDB_MAGIC, BDB_MAGIC_SIZE) != 0) {
        return bdb_error_set(err, BDB_ERR_FORMAT, "Invalid or corrupted file. '%s' is not a .bdb file", path);
    }
    if (version != BDB_VERSION) {
        return bdb_error_set(err, BDB_ERR_FORMAT, "Invalid or corrupted file '%s': Header version mismatch.", path);
    }

    //footer check
    bdb_fseek(reader->file, file_size - 12, SEEK_SET);
    if (fread(&footer_offset, sizeof(uint64_t), 1, reader->file) != 1) {
        return bdb_error_set(err, BDB_ERR_FORMAT, "Invalid or corrupted file '%s': Footer offset not found.", path);
    }
    if (fread(magic, 1, BDB_MAGIC_SIZE, reader->file) != BDB_MAGIC_SIZE || memcmp(magic, BDB_MAGIC, BDB_MAGIC_SIZE) != 0) {
        return bdb_error_set(err, BDB_ERR_FORMAT, "Invalid or corrupted file '%s': Footer magic mismatch.", path);
    }


    //seek to footer offset and start reading footer data
    bdb_fseek(reader->file, footer_offset, SEEK_SET);
    if (fread(&reader->row_count, sizeof(uint64_t), 1, reader->file) != 1){
        return bdb_error_set(err, BDB_ERR_FORMAT, "Invalid or corrupted file '%s': Footer row count not found.", path);
    }
    if (fread(&reader->col_count, sizeof(uint16_t), 1, reader->file) != 1){
        return bdb_error_set(err, BDB_ERR_FORMAT, "Invalid or corrupted file '%s': Footer col count not found.", path);
    }


    // One COLUMN per column. calloc sets every name/data/bitmap pointer to
    // NULL, so bdb_reader_close can free whatever was filled in, even if we
    // fail halfway through the loop below.
    reader->chunk.columns = calloc(reader->col_count, sizeof(COLUMN));
    if (reader->chunk.columns == NULL) {
        return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory for %u columns", reader->col_count);
    }
    // Set only once the array exists, so chunk_free never loops over NULL.
    reader->chunk.col_count = reader->col_count;

    for (uint16_t c = 0; c < reader->col_count; c++) {
        uint16_t name_len = 0;
        fread(&name_len, sizeof(uint16_t), 1, reader->file);

        char *name = malloc(name_len + 1);
        if (name == NULL) return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory for column name");

        if (fread(name, 1, name_len, reader->file) != name_len) {
            free(name);
            return bdb_error_set(err, BDB_ERR_FORMAT, "'%s': column %u name is cut short", path, c);
        }

        name[name_len] = '\0';
        reader->chunk.columns[c].name = name;

        uint8_t type;
        if (fread(&type, 1, 1, reader->file) != 1) {
            return bdb_error_set(err, BDB_ERR_FORMAT, "'%s': column %u type is missing", path, c);
        }
        if (type != BDB_COL_INT && type != BDB_COL_DOUBLE && type != BDB_COL_BOOL) {
            return bdb_error_set(err, BDB_ERR_FORMAT, "'%s': column %u has unknown type %u", path, c, type);
        }
        reader->chunk.columns[c].type = (enum ColumnType)type;
    }

    //Read group count
    if (fread(&reader->group_count, sizeof(uint32_t), 1, reader->file) != 1) {
        return bdb_error_set(err, BDB_ERR_FORMAT, "'%s': group count is missing", path);
    }

    reader->row_groups = malloc(reader->group_count * sizeof(uint32_t));
    reader->offsets = malloc(reader->group_count * reader->col_count * sizeof(uint64_t));

    for (int i = 0; i < reader->group_count; i++) {
        if (fread(&reader->row_groups[i], sizeof(uint32_t), 1, reader->file) != 1
            || fread(&reader->offsets[(size_t)i * reader->col_count], sizeof(uint64_t),
                      reader->col_count, reader->file) != reader->col_count) {
            return bdb_error_set(err, BDB_ERR_FORMAT, "'%s': group %u is cut short", path, i);
        }
    }


    for (uint16_t c = 0; c < reader->col_count; c++) {
        COLUMN *col = &reader->chunk.columns[c];
        col->data   = calloc(BDB_VECTOR_SIZE, bdb_col_type_size(col->type));
        col->bitmap = calloc(BDB_VECTOR_SIZE, 1);
        if (col->data == NULL || col->bitmap == NULL) {
          return bdb_error_set(err, BDB_ERR_NOMEM, "out of memory for column '%s'", col->name);
        }
    }

    return BDB_OK;
}

BdbStatus bdb_reader_next_chunk(BDB_READER *reader, const CHUNK **out, BdbError *err) {

    *out = NULL;
    chunk_reset(&reader->chunk);

    if (reader->group_index == reader->group_count) {
        return BDB_OK;                                   // no rows left
    }


    size_t n = reader->row_groups[reader->group_index];
    size_t left = n - reader->rows_in_group;
    size_t to_take = left > BDB_VECTOR_SIZE ? BDB_VECTOR_SIZE : left;


    for (uint16_t c = 0; c < reader->col_count; c++) {
        COLUMN  *col  = &reader->chunk.columns[c];
        uint64_t base = reader->offsets[(size_t)reader->group_index * reader->col_count + c];
        size_t   size = bdb_col_type_size(col->type);

        // bitmap: 1 byte per row, starting at this chunk's first row
        if (bdb_fseek(reader->file, (int64_t)(base + reader->rows_in_group), SEEK_SET) != 0 ||
            fread(col->bitmap, 1, to_take, reader->file) != to_take) {
            return bdb_error_set(err, BDB_ERR_IO, "could not read bitmap of column '%s'", col->name);
        }

        // values: after the whole bitmap (n bytes), then this chunk's first row
        if (bdb_fseek(reader->file, (int64_t)(base + n + reader->rows_in_group * size), SEEK_SET) != 0 ||
            fread(col->data, size, to_take, reader->file) != to_take) {
            return bdb_error_set(err, BDB_ERR_IO, "could not read values of column '%s'", col->name);
        }
    }

    reader->chunk.count = to_take;
    reader->rows_in_group += to_take;

    if (reader->rows_in_group == n) {
        reader->group_index++;
        reader->rows_in_group = 0;
    }

    *out = &reader->chunk;
    return BDB_OK;
}

void bdb_reader_close(BDB_READER *reader) {
    // Column names (and, later, the chunk buffers). Safe on a zeroed chunk.
    chunk_free(&reader->chunk);

    free(reader->row_groups);
    free(reader->offsets);
    reader->row_groups = NULL;
    reader->offsets = NULL;

    if (reader->file != NULL) {
        fclose(reader->file);
        reader->file = NULL;
    }

    reader->row_count = 0;
    reader->col_count = 0;
    reader->group_count = 0;
}
