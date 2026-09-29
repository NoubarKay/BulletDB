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

#endif //BULLETDB_IO_H
