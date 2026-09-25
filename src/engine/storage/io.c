#include <stdio.h>
#include "../common.h"
//
// Created by user on 9/25/2026.
//
BdbStatus read_bytes(FILE *file, void *destination, size_t item_size, size_t count, BdbError *err, const char *msg) {
    size_t got = fread(destination, item_size, count, file);
    if (got != count) {
        return bdb_error_set(err, BDB_ERR_IO, "%s", msg);
    }
    return BDB_OK;
}