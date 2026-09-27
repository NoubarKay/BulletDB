//
// Created by user on 9/27/2026.
//

#ifndef BULLETDB_BDB_SINK_H
#define BULLETDB_BDB_SINK_H
#include "../common.h"
#include "../chunk.h"

typedef BdbStatus (*BdbChunkFn)(const CHUNK *chunk, void *ctx, BdbError *err);

#endif //BULLETDB_BDB_SINK_H
