//
// Created by user on 9/25/2026.
//

#ifndef BULLETDB_IO_H
#define BULLETDB_IO_H
#include <stdio.h>
#include <stdint.h>
#include "common/common.h"

#ifdef _WIN32
#  include <windows.h>
#  define bdb_fseek(f, o, w)  _fseeki64((f), (o), (w))
#  define bdb_ftell(f)         _ftelli64(f)
static inline uint64_t bdb_now(void) {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000000000LL / freq.QuadPart);
}
#else
#  include <time.h>
#  define bdb_fseek(f, o, w)  fseeko((f), (off_t)(o), (w))
#  define bdb_ftell(f)         ftello(f)
static inline uint64_t bdb_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

BdbStatus read_bytes(FILE *file, void *destination, size_t item_size, size_t count, BdbError *err, const char *msg);

// Permissions for files BulletDB creates: read and write for the owner only,
// like PostgreSQL's data files. Used on POSIX systems.
#define BDB_FILE_MODE 0600

// Creates `path` (or empties it if it exists) and opens it for binary
// writing. On POSIX systems the file gets BDB_FILE_MODE explicitly, instead
// of fopen's 0666 minus the user's umask, so it's never readable or writable
// by other users, even with a permissive umask. An existing file is tightened
// to BDB_FILE_MODE too. On Windows, new files inherit their folder's ACLs, so
// this is plain fopen. Returns NULL on failure (errno is set), like fopen.
FILE *bdb_create_file(const char *path);

#endif //BULLETDB_IO_H
