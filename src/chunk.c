
#include "engine/chunk.h"

#include <stdlib.h>

BdbStatus chunk_init(CHUNK *c, uint64_t col_count, BdbError *err) {
    c->col_count = col_count;
    c->count = 0;
    c->columns = NULL;

    return BDB_OK;
}

void chunk_reset(CHUNK *c) {
    c->count = 0;
}

void chunk_free(CHUNK *c) {
    free(c->columns);
}
