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

BdbExpr bdb_expr_binary(enum BdbBinaryExprKind op, const BdbExpr *l, const BdbExpr *r) {
    BdbExpr expr = {0};
    expr.kind = BDB_EXPR_BINARY;
    expr.binary.op = op;
    expr.binary.l = l;
    expr.binary.r = r;
    return expr;
}

// The operator as it's written in a query, e.g. ">=".
static const char *bdb_binary_expr_kind_to_str(enum BdbBinaryExprKind op) {
    switch (op) {
        case BDB_BINARY_EXPR_EQ:  return "=";
        case BDB_BINARY_EXPR_NE:  return "!=";
        case BDB_BINARY_EXPR_LT:  return "<";
        case BDB_BINARY_EXPR_LE:  return "<=";
        case BDB_BINARY_EXPR_GT:  return ">";
        case BDB_BINARY_EXPR_GE:  return ">=";
        case BDB_BINARY_EXPR_ADD: return "+";
        case BDB_BINARY_EXPR_SUB: return "-";
        case BDB_BINARY_EXPR_MUL: return "*";
        case BDB_BINARY_EXPR_DIV: return "/";
    }
    return "?";
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
        case BDB_EXPR_BINARY:
            BdbField left, right;

            BdbStatus status = bdb_expr_to_field(expr->binary.l, schema, &left, err);
            if (status != BDB_OK) return status;
            status = bdb_expr_to_field(expr->binary.r, schema, &right, err);
            if (status != BDB_OK) return status;

            if (expr->binary.op == BDB_BINARY_EXPR_ADD || expr->binary.op == BDB_BINARY_EXPR_SUB || expr->binary.op == BDB_BINARY_EXPR_MUL || expr->binary.op == BDB_BINARY_EXPR_DIV) {
                if ((left.type != BDB_COL_DOUBLE && left.type != BDB_COL_INT) || (right.type != BDB_COL_DOUBLE && right.type != BDB_COL_INT)) {
                    return bdb_error_set(err, BDB_ERR_INVALID, "math needs INT or DOUBLE on both sides");
                }

                if (left.type == BDB_COL_DOUBLE || right.type == BDB_COL_DOUBLE) {
                    out_field->type = BDB_COL_DOUBLE;
                } else {
                    out_field->type = BDB_COL_INT;
                }

                out_field->name = bdb_binary_expr_kind_to_str(expr->binary.op);
                return BDB_OK;
            }else {
                out_field->name = bdb_binary_expr_kind_to_str(expr->binary.op);
                out_field->type = BDB_COL_BOOL;
                return BDB_OK;
            }
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

        case BDB_EXPR_BINARY:
            bdb_expr_print(expr->binary.l, out);
            fprintf(out, " %s ", bdb_binary_expr_kind_to_str(expr->binary.op));
            bdb_expr_print(expr->binary.r, out);
            break;
    }
}

