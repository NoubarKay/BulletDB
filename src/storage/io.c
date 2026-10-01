#include <stdio.h>
#include "common/common.h"
#include "storage/io.h"

#ifndef _WIN32
#  include <fcntl.h>      // open, O_* flags
#  include <sys/stat.h>   // fchmod
#  include <unistd.h>     // close
#endif
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

FILE *bdb_create_file(const char *path) {
#ifdef _WIN32
    return fopen(path, "wb");
#else
    // The mode only applies when the file is created...
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, BDB_FILE_MODE);
    if (fd < 0) return NULL;

    // ...so an existing file is tightened explicitly too.
    if (fchmod(fd, BDB_FILE_MODE) != 0) {
        close(fd);
        return NULL;
    }

    FILE *file = fdopen(fd, "wb");
    if (file == NULL) close(fd);
    return file;
#endif
}
