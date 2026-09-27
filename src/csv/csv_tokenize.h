//
// Created by user on 9/26/2026.
//

#include <stdio.h>
#include <stdint.h>
#include "common/common.h"

#ifndef BULLETDB_CSV_TOKENIZE_H
#define BULLETDB_CSV_TOKENIZE_H

#define DELIMS ",\r\n"
#define BDB_CSV_MAX_LINE (1024 * 1024)   // longest line accepted, in bytes
#define BDB_CSV_MAX_FIELD 255            // longest field accepted, in characters

char *next_field(char **cursor);
BdbStatus read_next_line(FILE *file, char *buffer, BdbError *err, uint64_t *line_no, char** dest);

#endif //BULLETDB_CSV_TOKENIZE_H
