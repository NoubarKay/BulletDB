//
// Created by user on 9/24/2026.
//
#pragma once

#ifndef BULLETDB_COMMON_H
#define BULLETDB_COMMON_H


typedef enum {
    BDB_OK = 0,
    BDB_ERR_OPEN,      // couldn't open a file
    BDB_ERR_NOMEM,     // malloc/calloc/realloc/strdup failed
    BDB_ERR_PARSE,     // bad value in the CSV (e.g. "abc" in a number column)
    BDB_ERR_IO,        // fread/fwrite read or wrote fewer items than asked
    BDB_ERR_FORMAT,    // file isn't a valid .bdb, or is corrupted
    BDB_ERR_NOT_FOUND,  // e.g. a column name that doesn't exist
    BDB_ERR_INVALID
} BdbStatus;

typedef struct {
    BdbStatus code;
    char message[256];
} BdbError;

// Short generic text for a status code.
const char *bdb_status_str(BdbStatus code);

// Fills in err (code + formatted message) and returns the code,
// so failure paths can do: return bdb_error_set(err, ...);
BdbStatus bdb_error_set(BdbError *err, BdbStatus code, const char *fmt, ...);

#endif //BULLETDB_COMMON_H
