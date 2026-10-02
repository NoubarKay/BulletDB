#include "logical/bdb_expr.h"

#include <inttypes.h>
#include <string.h>

BdbExpr bdb_expr_column(const char *col_name) {
    BdbExpr expr = {0};
    expr.kind = BDB_EXPR_COL;
    expr.col.name = col_name;
    return expr;
}

BdbExpr bdb_expr_literal_long(int64_t l) {
    BdbExpr expr = {0};
    expr.kind = BDB_EXPR_LITERAL_LONG;
    expr.literal_long.n = l;
    snprintf(expr.literal_long.text, sizeof expr.literal_long.text, "%" PRId64, l);
    return expr;
}

BdbExpr bdb_expr_literal_double(double d) {
    BdbExpr expr = {0};
    expr.kind = BDB_EXPR_LITERAL_DOUBLE;
    expr.literal_double.n = d;
    snprintf(expr.literal_double.text, sizeof expr.literal_double.text, "%g", d);
    return expr;
}

BdbExpr bdb_expr_literal_string(const char *str) {
    BdbExpr expr = {0};
    expr.kind = BDB_EXPR_LITERAL_STRING;
    expr.literal_string.str = str;
    return expr;
}


BdbStatus bdb_expr_to_field(const BdbExpr *expr, const BdbSchema *schema, BdbField *out_field, BdbError *err) {
    switch (expr->kind) {
        case BDB_EXPR_COL:
            // Find the field with this column's name in the input.
            for (uint16_t i = 0; i < schema->col_count; i++) {
                if (strcmp(schema->fields[i].name, expr->col.name) == 0) {
                    *out_field = schema->fields[i];
                    return BDB_OK;
                }
            }
            return bdb_error_set(err, BDB_ERR_NOT_FOUND, "no column named '%s'", expr->col.name);
        case BDB_EXPR_LITERAL_LONG:
            out_field->name = expr->literal_long.text;
            out_field->type = BDB_COL_INT;
            return BDB_OK;
        case BDB_EXPR_LITERAL_DOUBLE:
            out_field->name = expr->literal_double.text;
            out_field->type = BDB_COL_DOUBLE;
            return BDB_OK;
        case BDB_EXPR_LITERAL_STRING:
            out_field->name = expr->literal_string.str;
            out_field->type = BDB_COL_STR;
            return BDB_OK;

    }
    return bdb_error_set(err, BDB_ERR_INVALID, "unknown expression kind");
}

void bdb_expr_print(const BdbExpr *expr, FILE *out) {
    switch (expr->kind) {
        case BDB_EXPR_COL:
            fprintf(out, "#%s", expr->col.name);
            break;

        case BDB_EXPR_LITERAL_LONG:
            fprintf(out, "%s", expr->literal_long.text);
            break;

        case BDB_EXPR_LITERAL_DOUBLE:
            fprintf(out, "%s", expr->literal_double.text);
            break;

        case BDB_EXPR_LITERAL_STRING:
            fprintf(out, "'%s'", expr->literal_string.str);
            break;
    }
}

