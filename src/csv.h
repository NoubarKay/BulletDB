//
// Created by user on 9/24/2026.
//

#ifndef BULLETDB_CSV_H
#define BULLETDB_CSV_H
#include "engine/common.h"
#include "engine/sink/bdb_sink.h"

BdbStatus read_csv(const char *path, BdbChunkFn on_chunk, BdbError *err);

#endif //BULLETDB_CSV_H
