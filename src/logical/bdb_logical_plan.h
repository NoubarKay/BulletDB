//
// Created by user on 10/2/2026.
//

#ifndef BULLETDB_BDB_LOGICAL_PLAN_H
#define BULLETDB_BDB_LOGICAL_PLAN_H

#include "logical/bdb_schema.h"

enum BdbPlanKind {
    BDB_PLAN_SCAN
};

typedef struct BdbLogicalPlan {
    enum BdbPlanKind kind;
    BdbSchema schema;
    struct BdbLogicalPlan *child;
} BdbLogicalPlan;

#endif //BULLETDB_BDB_LOGICAL_PLAN_H
