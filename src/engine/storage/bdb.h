//
// Created by user on 9/24/2026.
//

#pragma once

#ifndef BULLETDB_BDB_H
#define BULLETDB_BDB_H
#include "../common.h"
#include "../table.h"

// Writes the table to a .bdb file at path.
BdbStatus bdb_write(const char *path, const TABLE *table, BdbError *err);

// Reads a .bdb file at path into table, which must start zeroed.
// On failure, table may be partly filled; bdb_table_free cleans it up.
BdbStatus bdb_read(const char *path, TABLE *table, BdbError *err);

#endif //BULLETDB_BDB_H
