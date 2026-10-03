#include <stdlib.h>

#include "executor/aggregate.h"
#include "executor/filter.h"
#include "logical/bdb_expr.h"
#include "planner/bdb_planner.h"

#include "executor/scan.h"

static BdbCompareOp to_compare_op(const enum BdbBinaryExprKind kind) {
    switch (kind) {
        case BDB_BINARY_EXPR_EQ:
            return BDB_COMPARE_EQ;
        case BDB_BINARY_EXPR_NE:
            return BDB_COMPARE_NE;
        case BDB_BINARY_EXPR_LT:
            return BDB_COMPARE_LT;
        case BDB_BINARY_EXPR_GT:
            return BDB_COMPARE_GT;
        case BDB_BINARY_EXPR_LE:
            return BDB_COMPARE_LE;
        case BDB_BINARY_EXPR_GE:
            return BDB_COMPARE_GE;
    }
    return BDB_COMPARE_EQ;
}


static BdbAggregateType to_aggregate_op(const enum BdbAggregateExprKind kind) {
    switch (kind) {
        case BDB_AGGREGATE_EXPR_SUM:
            return BDB_AGG_SUM;
        case BDB_AGGREGATE_EXPR_COUNT:
            return BDB_AGG_COUNT;
        case BDB_AGGREGATE_EXPR_MIN:
            return BDB_AGG_MIN;
        case BDB_AGGREGATE_EXPR_MAX:
            return BDB_AGG_MAX;
        case BDB_AGGREGATE_EXPR_AVG:
            return BDB_AGG_AVG;
    }
    return BDB_AGG_SUM;
}

void bdb_planner_free(BdbOperator *root) {
    if (root == NULL) return;

    // close() cascades: each operator closes its own child, and the scan at
    // the bottom closes the file. It doesn't free anything, though.
    root->close(root);

    // So free the chain from the top down, reading each child pointer before
    // its parent is freed. Each operator's base is its first member, so this
    // pointer is the one malloc returned for the whole struct.
    while (root != NULL) {
        BdbOperator *child = root->child;
        free(root);
        root = child;
    }
}

BdbStatus bdb_planner_create(const BdbLogicalPlan *plan, BdbOperator **out, BdbError *err) {
    switch (plan->kind) {
        case BDB_PLAN_SCAN:
            BdbScan *scan = malloc(sizeof *scan);
            if (scan == NULL) {
                return bdb_error_set(err, BDB_ERR_NOMEM, "not enough memory to open scan");
            }

            BdbStatus status = bdb_scan_open(scan, plan->scan.path, err);
            if (status != BDB_OK) {
                scan->base.close(&scan->base);
                free(scan);
                return status;
            }

            *out = &scan->base;
            return BDB_OK;
        case BDB_PLAN_SELECTION: {
            // BdbFilter can only run "column <comparison> number", so check the
            // condition has that shape before reading its parts. Done before
            // planning the child, so there's nothing to clean up on failure.
            const BdbExpr *condition = plan->selection.condition;
            if (condition->kind != BDB_EXPR_BINARY || condition->binary.op > BDB_BINARY_EXPR_GE) {
                return bdb_error_set(err, BDB_ERR_INVALID,
                                     "the engine can only filter with a comparison (=, !=, <, <=, >, >=)");
            }
            const BdbExpr *column = condition->binary.l;
            const BdbExpr *value  = condition->binary.r;
            if (column->kind != BDB_EXPR_COL) {
                return bdb_error_set(err, BDB_ERR_INVALID,
                                     "the engine can only filter a column against a number: "
                                     "the left side must be a column");
            }
            if (value->kind != BDB_EXPR_LITERAL_LONG && value->kind != BDB_EXPR_LITERAL_DOUBLE) {
                return bdb_error_set(err, BDB_ERR_INVALID,
                                     "the engine can only filter a column against a number: "
                                     "the right side must be a number");
            }

            BdbOperator *child = NULL;
            BdbStatus status = bdb_planner_create(plan->child, &child, err);
            if (status != BDB_OK) {
                return status;
            }

            BdbFilter *filter = malloc(sizeof *filter);
            if (filter == NULL) {
                bdb_planner_free(child);
                return bdb_error_set(err, BDB_ERR_NOMEM, "not enough memory to create filter");
            }

            // Take the condition apart into the three things BdbFilter wants:
            // the column's name, the comparison, and the number as text.
            const char *value_text = value->kind == BDB_EXPR_LITERAL_LONG
                                         ? value->literal_long.text
                                         : value->literal_double.text;
            bdb_filter_init(filter, child, column->col.name, to_compare_op(condition->binary.op), value_text);

            *out = &filter->base;
            return BDB_OK;
        }
        case BDB_PLAN_AGGREGATE:
            return bdb_error_set(err, BDB_ERR_INVALID, "the planner can't build an aggregate yet");
    }
    return bdb_error_set(err, BDB_ERR_INVALID, "unknown plan kind %d", (int)plan->kind);
}