//
// Created by user on 9/24/2026.
//
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/table.h"
#include "csv/csv_reader.h"
#include "common/common.h"
#include "executor/aggregate.h"
#include "executor/scan.h"
#include "executor/explain.h"
#include "executor/filter.h"
#include "logical/bdb_expr.h"
#include "storage/bdb_writer.h"

// Prints one chunk as a table.
static BdbStatus print_chunk_cb(const CHUNK *chunk, BdbError *err) {
    (void)err;


    // A chunk has the same shape as a table, so borrow print_table.
    TABLE view = {
        .row_count = chunk->count,
        .col_count = chunk->col_count,
        .columns = chunk->columns,
        .sel_vector = chunk->sel_vector,
    };
    print_table(&view);
    return BDB_OK;
}

// Prints one logical expression, then what bdb_expr_to_field says it produces
// against `schema`: a field's name and type, or the error.
static void show_expr(const char *label, const BdbExpr *expr, const BdbSchema *schema) {
    BdbField field;
    BdbError err = {0};

    printf("%-28s prints as  ", label);
    bdb_expr_print(expr, stdout);

    if (bdb_expr_to_field(expr, schema, &field, &err) != BDB_OK) {
        printf("\n%-28s error: %s\n\n", "", err.message);
        return;
    }
    const char *type = field.type == BDB_COL_INT    ? "INT"
                     : field.type == BDB_COL_DOUBLE ? "DOUBLE"
                     : field.type == BDB_COL_BOOL   ? "BOOL" : "STR";
    printf("\n%-28s field: name \"%s\", type %s\n\n", "", field.name, type);
}

// A tour of the logical expressions, checked against a made-up employees table.
static void show_logical_expressions(void) {
    BdbField fields[] = {
        { "name",       BDB_COL_STR },
        { "salary",     BDB_COL_DOUBLE },
        { "department", BDB_COL_STR },
        { "age",        BDB_COL_INT },
    };
    BdbSchema employees = { fields, 4 };

    BdbExpr salary      = bdb_expr_column("salary");
    BdbExpr age         = bdb_expr_column("age");
    BdbExpr missing     = bdb_expr_column("height");            // not in the table
    BdbExpr forty_five  = bdb_expr_literal_long(45);
    BdbExpr negative    = bdb_expr_literal_long(-7);
    BdbExpr factor      = bdb_expr_literal_double(1.1);
    BdbExpr engineering = bdb_expr_literal_string("Engineering");

    printf("== logical expressions ==\n\n");
    show_expr("column salary",                &salary,      &employees);
    show_expr("column age",                   &age,         &employees);
    show_expr("column height (missing)",      &missing,     &employees);
    show_expr("literal long 45",              &forty_five,  &employees);
    show_expr("literal long -7",              &negative,    &employees);
    show_expr("literal double 1.1",           &factor,      &employees);
    show_expr("literal string Engineering",   &engineering, &employees);

    // Binary expressions: two sides and an operator.
    BdbExpr department = bdb_expr_column("department");
    BdbExpr limit      = bdb_expr_literal_long(100000);
    BdbExpr one        = bdb_expr_literal_long(1);
    BdbExpr two        = bdb_expr_literal_long(2);

    BdbExpr older       = bdb_expr_binary(BDB_BINARY_EXPR_GE,  &age,        &forty_five);
    BdbExpr engineers   = bdb_expr_binary(BDB_BINARY_EXPR_EQ,  &department, &engineering);
    BdbExpr raised      = bdb_expr_binary(BDB_BINARY_EXPR_MUL, &salary,     &factor);
    BdbExpr rich        = bdb_expr_binary(BDB_BINARY_EXPR_GT,  &raised,     &limit);    // a binary inside a binary
    BdbExpr age_plus    = bdb_expr_binary(BDB_BINARY_EXPR_ADD, &age,        &one);
    BdbExpr doubled     = bdb_expr_binary(BDB_BINARY_EXPR_MUL, &age_plus,   &two);      // (age + 1) * 2
    BdbExpr age_scaled  = bdb_expr_binary(BDB_BINARY_EXPR_MUL, &age,        &factor);
    BdbExpr taller      = bdb_expr_binary(BDB_BINARY_EXPR_GE,  &missing,    &forty_five);

    printf("== binary expressions ==\n\n");
    show_expr("age >= 45",                    &older,       &employees);
    show_expr("department = 'Engineering'",   &engineers,   &employees);
    show_expr("salary * 1.1",                 &raised,      &employees);
    show_expr("salary * 1.1 > 100000",        &rich,        &employees);
    show_expr("(age + 1) * 2",                &doubled,     &employees);
    show_expr("age * 1.1",                    &age_scaled,  &employees);
    show_expr("height >= 45 (missing)",       &taller,      &employees);
}

// ---------------------------------------------------------------------------
// Checks for binary expressions: each one says PASS or FAIL, and why.
// ---------------------------------------------------------------------------

static int binary_failures = 0;

static const char *type_name(enum ColumnType type) {
    return type == BDB_COL_INT    ? "INT"
         : type == BDB_COL_DOUBLE ? "DOUBLE"
         : type == BDB_COL_BOOL   ? "BOOL" : "STR";
}

// Expects to_field to succeed with this name and type.
static void expect_field(const char *label, const BdbExpr *expr, const BdbSchema *schema,
                         const char *name, enum ColumnType type) {
    BdbField field;
    BdbError err = {0};

    if (bdb_expr_to_field(expr, schema, &field, &err) != BDB_OK) {
        printf("FAIL  %-34s expected %s \"%s\", got error: %s\n",
               label, type_name(type), name, err.message);
        binary_failures++;
    } else if (field.type != type || strcmp(field.name, name) != 0) {
        printf("FAIL  %-34s expected %s \"%s\", got %s \"%s\"\n",
               label, type_name(type), name, type_name(field.type), field.name);
        binary_failures++;
    } else {
        printf("PASS  %-34s %s \"%s\"\n", label, type_name(type), name);
    }
}

// Expects to_field to fail. If `mentions` isn't NULL, the error message must
// contain it (for example the name of the missing column).
static void expect_error(const char *label, const BdbExpr *expr, const BdbSchema *schema,
                         const char *mentions) {
    BdbField field;
    BdbError err = {0};

    if (bdb_expr_to_field(expr, schema, &field, &err) == BDB_OK) {
        printf("FAIL  %-34s expected an error, got %s \"%s\"\n",
               label, type_name(field.type), field.name);
        binary_failures++;
    } else if (mentions != NULL && strstr(err.message, mentions) == NULL) {
        printf("FAIL  %-34s error doesn't mention '%s': %s\n", label, mentions, err.message);
        binary_failures++;
    } else {
        printf("PASS  %-34s error: %s\n", label, err.message);
    }
}

static void test_binary_expressions(void) {
    BdbField fields[] = {
        { "name",       BDB_COL_STR },
        { "salary",     BDB_COL_DOUBLE },
        { "department", BDB_COL_STR },
        { "age",        BDB_COL_INT },
        { "active",     BDB_COL_BOOL },
    };
    BdbSchema employees = { fields, 5 };

    BdbExpr age         = bdb_expr_column("age");
    BdbExpr active      = bdb_expr_column("active");
    BdbExpr salary      = bdb_expr_column("salary");
    BdbExpr department  = bdb_expr_column("department");
    BdbExpr height      = bdb_expr_column("height");            // not in the table
    BdbExpr one         = bdb_expr_literal_long(1);
    BdbExpr two         = bdb_expr_literal_long(2);
    BdbExpr forty_five  = bdb_expr_literal_long(45);
    BdbExpr limit       = bdb_expr_literal_long(100000);
    BdbExpr factor      = bdb_expr_literal_double(1.1);
    BdbExpr engineering = bdb_expr_literal_string("Engineering");

    printf("== binary expression checks ==\n\n");

    // Every comparison gives a BOOL, named after its operator.
    printf("-- comparisons --\n");
    const enum BdbBinaryExprKind comparisons[] = {
        BDB_BINARY_EXPR_EQ, BDB_BINARY_EXPR_NE, BDB_BINARY_EXPR_LT,
        BDB_BINARY_EXPR_LE, BDB_BINARY_EXPR_GT, BDB_BINARY_EXPR_GE,
    };
    const char *symbols[] = { "=", "!=", "<", "<=", ">", ">=" };
    for (int i = 0; i < 6; i++) {
        BdbExpr compare = bdb_expr_binary(comparisons[i], &age, &forty_five);
        char label[64];
        snprintf(label, sizeof label, "age %s 45", symbols[i]);
        expect_field(label, &compare, &employees, symbols[i], BDB_COL_BOOL);
    }
    BdbExpr engineers = bdb_expr_binary(BDB_BINARY_EXPR_EQ, &department, &engineering);
    expect_field("department = 'Engineering'", &engineers, &employees, "=", BDB_COL_BOOL);

    // Math gives a number: DOUBLE if either side is DOUBLE, INT if both are INT.
    printf("\n-- math --\n");
    BdbExpr age_plus_one = bdb_expr_binary(BDB_BINARY_EXPR_ADD, &age, &one);
    BdbExpr age_div_two  = bdb_expr_binary(BDB_BINARY_EXPR_DIV, &age, &two);
    BdbExpr raised       = bdb_expr_binary(BDB_BINARY_EXPR_MUL, &salary, &factor);
    BdbExpr age_scaled   = bdb_expr_binary(BDB_BINARY_EXPR_MUL, &age, &factor);
    BdbExpr scaled_age   = bdb_expr_binary(BDB_BINARY_EXPR_MUL, &factor, &age);
    expect_field("age + 1          (INT, INT)",       &age_plus_one, &employees, "+", BDB_COL_INT);
    expect_field("age / 2          (INT, INT)",       &age_div_two,  &employees, "/", BDB_COL_INT);
    expect_field("salary * 1.1     (DOUBLE, DOUBLE)", &raised,       &employees, "*", BDB_COL_DOUBLE);
    expect_field("age * 1.1        (INT, DOUBLE)",    &age_scaled,   &employees, "*", BDB_COL_DOUBLE);
    expect_field("1.1 * age        (DOUBLE, INT)",    &scaled_age,   &employees, "*", BDB_COL_DOUBLE);

    // Binaries inside binaries.
    printf("\n-- nesting --\n");
    BdbExpr rich    = bdb_expr_binary(BDB_BINARY_EXPR_GT, &raised, &limit);
    BdbExpr doubled = bdb_expr_binary(BDB_BINARY_EXPR_MUL, &age_plus_one, &two);
    expect_field("salary * 1.1 > 100000", &rich,    &employees, ">", BDB_COL_BOOL);
    expect_field("(age + 1) * 2",         &doubled, &employees, "*", BDB_COL_INT);

    // A missing column is found on either side and at any depth.
    printf("\n-- errors --\n");
    BdbExpr left_missing   = bdb_expr_binary(BDB_BINARY_EXPR_GE,  &height, &forty_five);
    BdbExpr right_missing  = bdb_expr_binary(BDB_BINARY_EXPR_LE,  &forty_five, &height);
    BdbExpr height_plus    = bdb_expr_binary(BDB_BINARY_EXPR_ADD, &height, &one);
    BdbExpr deep_missing   = bdb_expr_binary(BDB_BINARY_EXPR_MUL, &height_plus, &two);
    BdbExpr string_math    = bdb_expr_binary(BDB_BINARY_EXPR_MUL, &department, &two);
    expect_error("height >= 45     (missing, left)",  &left_missing,  &employees, "height");
    expect_error("45 <= height     (missing, right)", &right_missing, &employees, "height");
    expect_error("(height + 1) * 2 (missing, deep)",  &deep_missing,  &employees, "height");
    BdbExpr bool_math      = bdb_expr_binary(BDB_BINARY_EXPR_ADD, &active, &one);
    BdbExpr bool_math_rhs  = bdb_expr_binary(BDB_BINARY_EXPR_ADD, &one, &active);
    expect_error("department * 2   (math on STR)",    &string_math,   &employees, NULL);
    expect_error("active + 1       (math on BOOL)",   &bool_math,     &employees, NULL);
    expect_error("1 + active       (BOOL on right)",  &bool_math_rhs, &employees, NULL);

    printf("\n%s: %d failure%s\n\n",
           binary_failures == 0 ? "all binary checks passed" : "binary checks FAILED",
           binary_failures, binary_failures == 1 ? "" : "s");
}

// Checks for aggregate expressions. They reuse expect_field and expect_error,
// so their failures are added to the same counter; the summary at the end
// counts only the ones from this function.
static void test_aggregate_expressions(void) {
    BdbField fields[] = {
        { "name",       BDB_COL_STR },
        { "salary",     BDB_COL_DOUBLE },
        { "department", BDB_COL_STR },
        { "age",        BDB_COL_INT },
        { "active",     BDB_COL_BOOL },
    };
    BdbSchema employees = { fields, 5 };
    int failures_before = binary_failures;

    BdbExpr name   = bdb_expr_column("name");
    BdbExpr salary = bdb_expr_column("salary");
    BdbExpr age    = bdb_expr_column("age");
    BdbExpr active = bdb_expr_column("active");
    BdbExpr height = bdb_expr_column("height");             // not in the table
    BdbExpr factor = bdb_expr_literal_double(1.1);
    BdbExpr raised = bdb_expr_binary(BDB_BINARY_EXPR_MUL, &salary, &factor);

    printf("== aggregate expression checks ==\n\n");

    // SUM, MIN and MAX keep the input's type; AVG is always DOUBLE.
    printf("-- types --\n");
    BdbExpr sum_salary  = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_SUM,   &salary);
    BdbExpr sum_age     = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_SUM,   &age);
    BdbExpr min_age     = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_MIN,   &age);
    BdbExpr max_salary  = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_MAX,   &salary);
    BdbExpr avg_age     = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_AVG,   &age);
    BdbExpr avg_salary  = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_AVG,   &salary);
    expect_field("SUM(salary)",        &sum_salary, &employees, "SUM", BDB_COL_DOUBLE);
    expect_field("SUM(age)",           &sum_age,    &employees, "SUM", BDB_COL_INT);
    expect_field("MIN(age)",           &min_age,    &employees, "MIN", BDB_COL_INT);
    expect_field("MAX(salary)",        &max_salary, &employees, "MAX", BDB_COL_DOUBLE);
    expect_field("AVG(age)    (INT in)", &avg_age,  &employees, "AVG", BDB_COL_DOUBLE);
    expect_field("AVG(salary)",        &avg_salary, &employees, "AVG", BDB_COL_DOUBLE);

    // COUNT is INT and accepts any type.
    printf("\n-- COUNT --\n");
    BdbExpr count_name   = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_COUNT, &name);
    BdbExpr count_active = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_COUNT, &active);
    BdbExpr count_salary = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_COUNT, &salary);
    expect_field("COUNT(name)   (STR in)",    &count_name,   &employees, "COUNT", BDB_COL_INT);
    expect_field("COUNT(active) (BOOL in)",   &count_active, &employees, "COUNT", BDB_COL_INT);
    expect_field("COUNT(salary) (DOUBLE in)", &count_salary, &employees, "COUNT", BDB_COL_INT);

    // The input can be any expression, not only a column.
    printf("\n-- an expression as input --\n");
    BdbExpr max_raised = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_MAX, &raised);
    BdbExpr avg_raised = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_AVG, &raised);
    expect_field("MAX(salary * 1.1)", &max_raised, &employees, "MAX", BDB_COL_DOUBLE);
    expect_field("AVG(salary * 1.1)", &avg_raised, &employees, "AVG", BDB_COL_DOUBLE);

    // Non-numbers are rejected by everything except COUNT, and a missing
    // column is reported by all of them.
    printf("\n-- errors --\n");
    BdbExpr sum_name     = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_SUM,   &name);
    BdbExpr min_name     = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_MIN,   &name);
    BdbExpr avg_active   = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_AVG,   &active);
    BdbExpr avg_name     = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_AVG,   &name);
    BdbExpr count_height = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_COUNT, &height);
    BdbExpr avg_height   = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_AVG,   &height);
    BdbExpr sum_height   = bdb_expr_aggregate(BDB_AGGREGATE_EXPR_SUM,   &height);
    expect_error("SUM(name)     (STR)",          &sum_name,     &employees, NULL);
    expect_error("MIN(name)     (STR)",          &min_name,     &employees, NULL);
    expect_error("AVG(active)   (BOOL)",         &avg_active,   &employees, NULL);
    expect_error("AVG(name)     (STR)",          &avg_name,     &employees, NULL);
    expect_error("COUNT(height) (missing)",      &count_height, &employees, "height");
    expect_error("AVG(height)   (missing)",      &avg_height,   &employees, "height");
    expect_error("SUM(height)   (missing)",      &sum_height,   &employees, "height");

    int failures = binary_failures - failures_before;
    printf("\n%s: %d failure%s\n\n",
           failures == 0 ? "all aggregate checks passed" : "aggregate checks FAILED",
           failures, failures == 1 ? "" : "s");
}

int main(int argc, char *argv[]) {
    show_logical_expressions();
    test_binary_expressions();
    test_aggregate_expressions();

    BdbError err = {0};
    CSV_READER csvReader = {0};
    BDB_WRITER writer = {0};
    BdbScan scan; BdbFilter filter; BdbFilter filter2; BdbAggregate agg;

    const CHUNK *chunk;
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "Usage: %s <file.csv> [column to SUM]\n", argv[0]);
        return 1;
    }


    BdbStatus status = csv_open(&csvReader, argv[1], &err);

    if (status != BDB_OK) {
        fprintf(stderr, "error: %s\n", err.message);
        return 1;
    }

    status = bdb_writer_open(&writer, "test-1.bdb", &err);

    if (status != BDB_OK) {
        fprintf(stderr, "error: %s\n", err.message);
        csv_close(&csvReader);
        return 1;
    }

    while ((status = csv_next_chunk(&csvReader, &chunk, &err)) == BDB_OK && chunk != NULL) {
        status = bdb_writer_append(&writer, chunk, &err);
        if (status != BDB_OK) break;
    }

    if (status == BDB_OK) {
        status = bdb_writer_finish(&writer, &err);
    }
    if (status != BDB_OK) fprintf(stderr, "error: %s\n", err.message);


    csv_close(&csvReader);
    bdb_writer_close(&writer);



    status = bdb_scan_open(&scan, "test-1.bdb", &err);
    bdb_filter_init(&filter, &scan.base, "QUANTITYORDERED", BDB_COMPARE_GE, "30");
    bdb_filter_init(&filter2, &filter.base, "QUANTITYORDERED", BDB_COMPARE_LT, "45");
    bdb_aggregate_init(&agg, &filter2.base, BDB_AGG_AVG, "QUANTITYORDERED");
    BdbOperator *op = &agg.base;

    while (status == BDB_OK && (status = bdb_op_next(op, &chunk, &err)) == BDB_OK && chunk != NULL) {
        if (chunk->count == 0) continue;
        print_chunk_cb(chunk, &err);
    }
    if (status != BDB_OK) fprintf(stderr, "error: %s\n", err.message);

    bdb_explain(op, stdout);

    op->close(op);


    return status == BDB_OK ? 0 : 1;


    // status = bdb_write("test.bdb", &table, &err);
    // if (status != BDB_OK) {
    //     fprintf(stderr, "error: %s\n", err.message);
    //     free_table(&table);
    //     return 1;
    // }
    // printf("\nWrote test.bdb\n");

    // // Read into a separate table so the CSV table isn't overwritten.
    // // bdb_read only loads the counts for now (no columns), so compare those
    // // instead of calling print_table/free_table on it.
    // TABLE loaded = {0};
    // status = bdb_read("test.bdb", &loaded, &err);
    // if (status != BDB_OK) {
    //     fprintf(stderr, "error: %s\n", err.message);
    //     free_table(&table);
    //     free_table(&loaded);
    //     return 1;
    // }
    //
    // printf("Read test.bdb: %" PRIu64 " rows, %" PRIu64 " columns\n",
    //        loaded.row_count, loaded.col_count);
    //
    // int matches = loaded.row_count == table.row_count &&
    //               loaded.col_count == table.col_count;
    // printf("Round trip: %s\n", matches ? "OK" : "MISMATCH");
    //
    // print_table(&loaded);
    //
    // size_t col_idx = 0;
    // int64_t result = 0;
    //
    // BdbQuery query = {
    //     BDB_AGG_COUNT,
    //     "price",
    //     true,
    //     {
    //         "year",
    //         BDB_OP_GE,
    //         2024
    //     }
    // };
    //
    // status = bdb_run_query(&loaded, &query, &result, &err);
    //
    // if (status != BDB_OK) {
    //     fprintf(stderr, "error: %s\n", err.message);
    //     free_table(&table);
    //     free_table(&loaded);
    //     return 1;
    // }
    //
    // printf("Sum of prices: %" PRId64 "\n", result);
    //

    //free_table(&loaded)
}


