//
// Created by user on 10/1/2026.
//

#ifndef BULLETDB_CSV_SNIFFER_H
#define BULLETDB_CSV_SNIFFER_H

#include <stdint.h>
#include <stdio.h>

#include "common/common.h"
#include "core/chunk.h"

// How many data rows the sniffer reads to decide the column types (15 chunks).
#define BDB_CSV_SAMPLE_ROWS 30720

// Reads up to BDB_CSV_SAMPLE_ROWS data rows from `f` (just after the header)
// and sets chunk->columns[c].type for every column: BOOL, INT or DOUBLE, the
// narrowest type that fits every non-empty value in the sample. A column that
// is empty in every sampled row is DOUBLE. Text is an error. Advances
// *line_no; the caller seeks back to the first data row afterwards.
BdbStatus csv_sniff(FILE *f, char *buffer, CHUNK *chunk, uint64_t *line_no, BdbError *err);

#endif //BULLETDB_CSV_SNIFFER_H
