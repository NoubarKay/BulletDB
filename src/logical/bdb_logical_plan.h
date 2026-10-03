//
// Created by user on 10/2/2026.
//

#ifndef BULLETDB_BDB_LOGICAL_PLAN_H
#define BULLETDB_BDB_LOGICAL_PLAN_H

#include <stdio.h>

#include "logical/bdb_expr.h"
#include "logical/bdb_schema.h"

enum BdbPlanKind {
    BDB_PLAN_SCAN,
    BDB_PLAN_SELECTION,
    BDB_PLAN_AGGREGATE
};

typedef struct BdbLogicalPlan {
    enum BdbPlanKind kind;
    BdbSchema schema;
    struct BdbLogicalPlan *child;

    union {
        struct { const char *path; } scan;     // BDB_PLAN_SCAN
        struct { const BdbExpr *condition; } selection;
        struct { const BdbExpr **exprs; uint16_t count; } aggregate;
    };
} BdbLogicalPlan;

void bdb_plan_print(const BdbLogicalPlan *plan, FILE *out);

BdbLogicalPlan bdb_plan_scan(const char *path, BdbSchema schema);

BdbStatus bdb_plan_selection(BdbLogicalPlan *out, BdbLogicalPlan *child, const BdbExpr *condition, BdbError *err);

BdbStatus bdb_plan_aggregate(BdbLogicalPlan *out, BdbLogicalPlan *child, const BdbExpr **exprs, uint16_t count, BdbField *fields, BdbError *err);

#endif //BULLETDB_BDB_LOGICAL_PLAN_H
