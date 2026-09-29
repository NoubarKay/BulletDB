//
// Created by user on 9/29/2026.
//

#ifndef BULLETDB_SCAN_H
#define BULLETDB_SCAN_H
#include "storage/bdb_reader.h"
#include "executor/bdb_operator.h"

typedef struct {
    BdbOperator base;
    BDB_READER reader;
} BdbScan;

BdbStatus bdb_scan_open(BdbScan *scan, const char *path, BdbError *err);


#endif //BULLETDB_SCAN_H
