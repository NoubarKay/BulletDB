//
// Created by user on 10/3/2026.
//

#ifndef BULLETDB_BDB_EXPR_H
#define BULLETDB_BDB_EXPR_H
#include <stdio.h>

#include "common/common.h"
#include "logical/bdb_field.h"
#include "logical/bdb_schema.h"

enum BdbExprKind {
    BDB_EXPR_COL,
    BDB_EXPR_LITERAL_LONG,
    BDB_EXPR_LITERAL_DOUBLE,
    BDB_EXPR_LITERAL_STRING
};

// A logical expression: describes a value during planning. It can't be run.
typedef struct {
    enum BdbExprKind kind;
    union {
        struct { const char *name; } col;     // BDB_EXPR_COL
        struct { int64_t n; char text[32]; } literal_long; //BDB_EXPR_LITERAL_LONG
        struct { double n; char text[32]; } literal_double; //BDB_EXPR_LITERAL_DOUBLE
        struct { const char *str; } literal_string; //BDB_EXPR_LITERAL_STRING
    };
} BdbExpr;

// A reference to a column of the input, by name. The name isn't copied.
BdbExpr bdb_expr_column(const char *col_name);

// A literal long value.
BdbExpr bdb_expr_literal_long(int64_t l);

// A literal double value
BdbExpr bdb_expr_literal_double(double d);

// A literal string value
BdbExpr bdb_expr_literal_string(const char *str);

// The name and type this expression produces when run against `schema`.
BdbStatus bdb_expr_to_field(const BdbExpr *expr, const BdbSchema *schema, BdbField *out_field, BdbError *err);

// Prints the expression as it appears in a plan, e.g. #salary.
void bdb_expr_print(const BdbExpr *expr, FILE *out);

#endif //BULLETDB_BDB_EXPR_H
