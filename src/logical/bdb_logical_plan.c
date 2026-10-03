
#include "logical/bdb_logical_plan.h"

static void print_plan(const BdbLogicalPlan *plan, FILE *out, int depth) {
    fprintf(out, "%*s", depth * 2, "");
    switch (plan->kind) {
        case BDB_PLAN_SCAN:
            fprintf(out, "SCAN: %s (%u columns)\n", plan->scan.path, plan->schema.col_count);
            break;
        case BDB_PLAN_SELECTION:
            fprintf(out, "FILTER: ");
            bdb_expr_print(plan->selection.condition, out);
            fprintf(out, "\n");
            print_plan(plan->child, out, depth + 1);
            break;
        case BDB_PLAN_AGGREGATE:
            fprintf(out, "AGGREGATE: ");
            for (uint16_t i = 0; i < plan->aggregate.count; i++) {
                bdb_expr_print(plan->aggregate.exprs[i], out);
                fprintf(out, " ");
            }
            fprintf(out, "\n");
            print_plan(plan->child, out, depth + 1);
            break;
    }
}

void bdb_plan_print(const BdbLogicalPlan *plan, FILE *out) {
    print_plan(plan, out, 0);
}

BdbLogicalPlan bdb_plan_scan(const char *path, BdbSchema schema) {
    BdbLogicalPlan plan = {0};

    plan.kind = BDB_PLAN_SCAN;
    plan.schema = schema;

    plan.scan.path = path;
    return plan;
}


BdbStatus bdb_plan_selection(BdbLogicalPlan *out, BdbLogicalPlan *child, const BdbExpr *condition, BdbError *err) {

    BdbField field = {0};
    BdbStatus status = bdb_expr_to_field(condition, &child->schema, &field, err);

    if (status != BDB_OK)
        return status;

    if (field.type != BDB_COL_BOOL)
        return bdb_error_set(err, BDB_ERR_INVALID, "selection condition must be a boolean expression");

    *out = (BdbLogicalPlan){0};

    out->kind = BDB_PLAN_SELECTION;
    out->child = child;
    out->schema = child->schema;

    out->selection.condition = condition;

    return BDB_OK;
}

BdbStatus bdb_plan_aggregate(BdbLogicalPlan *out, BdbLogicalPlan *child, const BdbExpr **exprs, uint16_t count, BdbField *fields, BdbError *err) {
    for (uint16_t i = 0; i < count; i++) {

        if (exprs[i]->kind != BDB_EXPR_AGGREGATE) {
            return bdb_error_set(err, BDB_ERR_INVALID, "aggregate expression must be an aggregate function");
        }

        BdbStatus status = bdb_expr_to_field(exprs[i], &child->schema, &fields[i], err);

        if (status != BDB_OK) {
            return status;
        }
    }

    *out = (BdbLogicalPlan){0};

    out->kind = BDB_PLAN_AGGREGATE;
    out->child = child;
    out->schema = (BdbSchema){ fields, count };
    out->aggregate.exprs = exprs;
    out->aggregate.count = count;

    return BDB_OK;
}