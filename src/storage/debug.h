//
// Created by user on 9/29/2026.
//

// Debug helpers for inspecting the storage layer. Not used by the engine
// itself; call them from main or a test while developing.

#ifndef BULLETDB_DEBUG_H
#define BULLETDB_DEBUG_H
#include <stdio.h>

#include "storage/bdb_writer.h"

// Prints the writer's schema, each group's row count and each column's block
// offset, and checks that every block starts where the previous one ended.
// Call it after bdb_writer_finish (before bdb_writer_close frees the arrays),
// or at any point while writing.
void bdb_writer_debug_dump(const BDB_WRITER *w, FILE *out);

#endif //BULLETDB_DEBUG_H
