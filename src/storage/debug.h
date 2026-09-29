//
// Created by user on 9/29/2026.
//

// Debug helpers for inspecting the storage layer. Not used by the engine
// itself; call them from main or a test while developing.

#ifndef BULLETDB_DEBUG_H
#define BULLETDB_DEBUG_H
#include <stdio.h>

#include "storage/bdb_reader.h"
#include "storage/bdb_writer.h"

// Prints the writer's schema, each group's row count and each column's block
// offset, and checks that every block starts where the previous one ended.
// Call it after bdb_writer_finish (before bdb_writer_close frees the arrays),
// or at any point while writing.
void bdb_writer_debug_dump(const BDB_WRITER *w, FILE *out);

// Prints what bdb_reader_open loaded from the footer (row count, schema, each
// group's row count and column offsets) and checks it for consistency.
// Problems are printed with a leading "!!". Returns the number of problems
// found (0 = the footer is consistent). Its output is laid out like the
// writer's dump, so the two can be compared line by line.
int bdb_reader_debug_dump(const BDB_READER *r, FILE *out);

#endif //BULLETDB_DEBUG_H
