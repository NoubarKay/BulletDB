//
// Created by user on 10/3/2026.
//

#ifndef BULLETDB_BDB_PLANNER_H
#define BULLETDB_BDB_PLANNER_H
#include "logical/bdb_logical_plan.h"
#include "executor/bdb_operator.h"

// Builds the executable operators for `plan`, bottom up, and returns the top
// one in *out. Every operator is malloc'd: release the tree with
// bdb_planner_free when the query is done.
BdbStatus bdb_planner_create(const BdbLogicalPlan *plan, BdbOperator **out, BdbError *err);

// Closes the operator tree (which closes the scan's file) and frees every
// operator in it. Safe to call with NULL.
void bdb_planner_free(BdbOperator *root);

#endif //BULLETDB_BDB_PLANNER_H
