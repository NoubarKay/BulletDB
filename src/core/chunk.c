
#include "core/chunk.h"

#include <stdlib.h>

BdbStatus chunk_init(CHUNK *c, uint64_t col_count) {
    c->col_count = col_count;
    c->count = 0;
    c->columns = NULL;
    c->sel_vector = NULL;

    return BDB_OK;
}

void chunk_reset(CHUNK *c) {
    c->count = 0;
    c->sel_vector = NULL;
}

void chunk_free(CHUNK *c) {
    for (uint64_t i = 0; i < c->col_count; i++) {
        free(c->columns[i].name);
        free(c->columns[i].data);
        free(c->columns[i].bitmap);
    }
    free(c->columns);
    c->columns = NULL;   // so a second chunk_free does nothing
    c->col_count = 0;
    c->count = 0;
    c->sel_vector = NULL;
}
