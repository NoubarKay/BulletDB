//
// Created by user on 9/24/2026.
//

#include <stdio.h>
#include "common.h"

#include <stdarg.h>

const char *bdb_status_str(BdbStatus code) {
    switch (code) {
        case BDB_OK:            return "ok";
        case BDB_ERR_OPEN:      return "could not open file";
        case BDB_ERR_NOMEM:     return "out of memory";
        case BDB_ERR_PARSE:     return "parse error";
        case BDB_ERR_IO:        return "I/O error";
        case BDB_ERR_FORMAT:    return "invalid or corrupted file";
        case BDB_ERR_NOT_FOUND: return "not found";
        default:                return "unknown error";
    }
}

BdbStatus bdb_error_set(BdbError *err, BdbStatus code, const char *fmt, ...) {
    if (err != NULL) {
        err->code = code;
        va_list args;
        va_start(args, fmt);
        vsnprintf(err->message, sizeof(err->message), fmt, args);
        va_end(args);
    }
    return code;
}

