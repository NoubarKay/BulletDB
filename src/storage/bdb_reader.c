#include <stdio.h>

#include "common/common.h"
#include "bdb_reader.h"

#include <stdlib.h>
#include <string.h>

#include "bdb_format.h"

BdbStatus bdb_reader_open(BDB_READER *reader, const char *path, BdbError *err) {
    *reader = (BDB_READER){0};

    BdbStatus status;

    reader->file = fopen(path, "rb");
    if (reader->file == NULL) {
        return bdb_error_set(err, BDB_ERR_OPEN, "could not open '%s'", path);
    }

    int64_t file_size;
    if (_fseeki64(reader->file, 0, SEEK_END) != 0) {
        return bdb_error_set(err, BDB_ERR_IO, "could not seek to end of file '%s'", path);
    }
    file_size = _ftelli64(reader->file);
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
    fseek(reader->file, file_size - 12, SEEK_SET);
    if (fread(&footer_offset, sizeof(uint64_t), 1, reader->file) != 1) {
        return bdb_error_set(err, BDB_ERR_FORMAT, "Invalid or corrupted file '%s': Footer offset not found.", path);
    }
    if (fread(magic, 1, BDB_MAGIC_SIZE, reader->file) != BDB_MAGIC_SIZE || memcmp(magic, BDB_MAGIC, BDB_MAGIC_SIZE) != 0) {
        return bdb_error_set(err, BDB_ERR_FORMAT, "Invalid or corrupted file '%s': Footer magic mismatch.", path);
    }


    //seek to footer offset and start reading footer data
    fseek(reader->file, footer_offset, SEEK_SET);
    if (fread(&reader->row_count, sizeof(uint64_t), 1, reader->file) != 1){
        return bdb_error_set(err, BDB_ERR_FORMAT, "Invalid or corrupted file '%s': Footer row count not found.", path);
    }
    if (fread(&reader->col_count, sizeof(uint16_t), 1, reader->file) != 1){
        return bdb_error_set(err, BDB_ERR_FORMAT, "Invalid or corrupted file '%s': Footer col count not found.", path);
    }


    //Initialize the chunk
    status = chunk_init(&reader->chunk, reader->col_count);
    if (status != BDB_OK) {
        return status;
    }

    reader->chunk.col_count = reader->col_count;

    COLUMN *temp = realloc(reader->chunk.columns, sizeof(COLUMN) * (uint16_t)(&reader->chunk.col_count + 1));

    if (temp == NULL) {
        return bdb_error_set(err, BDB_ERR_INVALID, "Failed to allocate memory for chunk columns.");
    }

    reader->chunk.columns = temp;

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


    return BDB_OK;
}
