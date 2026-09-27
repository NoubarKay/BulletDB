//
// Created by user on 9/26/2026.
//

#include <stdio.h>
#include <stdint.h>
#include "../common.h"

#ifndef BULLETDB_CSV_TOKENIZE_H
#define BULLETDB_CSV_TOKENIZE_H

char *next_field(char **cursor);
BdbStatus read_next_line(FILE *file, char *buffer, BdbError *err, uint64_t *line_no, char** dest);

#endif //BULLETDB_CSV_TOKENIZE_H
