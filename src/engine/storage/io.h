//
// Created by user on 9/25/2026.
//

#ifndef BULLETDB_IO_H
#define BULLETDB_IO_H
#include <stdio.h>
#include "../common.h"

BdbStatus read_bytes(FILE *file, void *destination, size_t item_size, size_t count, BdbError *err, const char *msg);

#endif //BULLETDB_IO_H
