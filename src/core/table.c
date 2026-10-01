//
// Created by user on 9/24/2026.
//
#include "core/table.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/common.h"





size_t bdb_col_type_size(enum ColumnType type) {
    switch (type) {
        case BDB_COL_INT:
            return sizeof(int64_t);
        case BDB_COL_DOUBLE:
            return sizeof(double);
        case BDB_COL_BOOL:
            return sizeof(bool);
        default:
            return 0;
    }
}

char* bdb_col_type_str(enum ColumnType type) {
    switch (type) {
        case BDB_COL_INT:
            return "int";
        case BDB_COL_DOUBLE:
            return "double";
        case BDB_COL_BOOL:
            return "bool";
        default:
            return "unknown";
    }
}

// Tables longer than 2 * PRINT_EDGE_ROWS show only the first and last
// PRINT_EDGE_ROWS rows, with a "..." row in between.
#define PRINT_EDGE_ROWS 10
#define PRINT_CELL_MAX 64

static bool is_numeric(enum ColumnType type) {
    return type == BDB_COL_INT || type == BDB_COL_DOUBLE;
}

// Formats one cell into out. A NULL bitmap means every row is valid.
// ReSharper disable once CppDFAConstantParameter
static void format_cell(const COLUMN *column, uint64_t row, char *out, size_t n) {
    if (column->bitmap != NULL && column->bitmap[row] == 0) {
        snprintf(out, n, "NULL");
        return;
    }
    if (column->data == NULL) {
        snprintf(out, n, "?");
        return;
    }
    switch (column->type) {
        case BDB_COL_INT:
            snprintf(out, n, "%" PRId64, ((int64_t *)column->data)[row]);
            break;
        case BDB_COL_DOUBLE:
            snprintf(out, n, "%.2f", ((double *)column->data)[row]);
            break;
        case BDB_COL_BOOL:
            snprintf(out, n, "%s", ((bool *)column->data)[row] ? "true" : "false");
            break;
        default:
            snprintf(out, n, "?");
            break;
    }
}

static uint64_t physical_row(const TABLE *t, uint64_t i) {
    return t->sel_vector ? t->sel_vector[i] : i;
}

// num_width is the width of the row-number column, printed before the others.
static void print_border(size_t num_width, const size_t *widths, uint64_t col_count) {
    putchar('+');
    for (size_t i = 0; i < num_width + 2; i++) putchar('-');
    putchar('+');
    for (uint64_t col = 0; col < col_count; col++) {
        for (size_t i = 0; i < widths[col] + 2; i++) putchar('-');
        putchar('+');
    }
    putchar('\n');
}

// Prints the row number (1, 2, 3... in display order, also for a filtered
// chunk), then every column's value.
static void print_row(const TABLE *table, size_t num_width, const size_t *widths, uint64_t row) {
    char cell[PRINT_CELL_MAX];
    printf("| %*" PRIu64 " |", (int)num_width, row + 1);
    for (uint64_t col = 0; col < table->col_count; col++) {
        const COLUMN *column = &table->columns[col];
        format_cell(column, physical_row(table, row), cell, sizeof(cell));
        if (is_numeric(column->type)) {
            printf(" %*s |", (int)widths[col], cell);
        } else {
            printf(" %-*s |", (int)widths[col], cell);
        }
    }
    putchar('\n');
}

// True if row is one of the rows print_table shows.
static bool is_shown(const TABLE *table, uint64_t row) {
    if (table->row_count <= 2 * PRINT_EDGE_ROWS) return true;
    return row < PRINT_EDGE_ROWS || row >= table->row_count - PRINT_EDGE_ROWS;
}

void print_table(TABLE *table) {
    if (table->col_count == 0) {
        printf("(0 rows, 0 columns)\n");
        return;
    }

    size_t *widths = calloc(table->col_count, sizeof(size_t));
    if (widths == NULL) {
        fprintf(stderr, "print_table: out of memory\n");
        return;
    }

    // Pass 1: each column is as wide as its longest name, type or shown value.
    // The row-number column fits "#", the largest shown row number, and the
    // "..." row if there is one.
    char cell[PRINT_CELL_MAX];
    size_t num_width = strlen("#");
    if (table->row_count > 2 * PRINT_EDGE_ROWS) num_width = strlen("...");
    for (uint64_t row = 0; row < table->row_count; row++) {
        if (!is_shown(table, row)) continue;
        snprintf(cell, sizeof(cell), "%" PRIu64, row + 1);
        size_t len = strlen(cell);
        if (len > num_width) num_width = len;
    }

    for (uint64_t col = 0; col < table->col_count; col++) {
        const COLUMN *column = &table->columns[col];
        size_t width = strlen(column->name);
        size_t type_width = strlen(bdb_col_type_str(column->type));
        if (type_width > width) width = type_width;

        for (uint64_t row = 0; row < table->row_count; row++) {
            if (!is_shown(table, row)) continue;
            format_cell(column, physical_row(table, row), cell, sizeof(cell));
            size_t len = strlen(cell);
            if (len > width) width = len;
        }
        widths[col] = width;
    }

    // Pass 2: print.
    print_border(num_width, widths, table->col_count);

    printf("| %*s |", (int)num_width, "#");
    for (uint64_t col = 0; col < table->col_count; col++) {
        printf(" %-*s |", (int)widths[col], table->columns[col].name);
    }
    putchar('\n');

    printf("| %*s |", (int)num_width, "");
    for (uint64_t col = 0; col < table->col_count; col++) {
        printf(" %-*s |", (int)widths[col], bdb_col_type_str(table->columns[col].type));
    }
    putchar('\n');

    print_border(num_width, widths, table->col_count);

    for (uint64_t row = 0; row < table->row_count; row++) {
        if (!is_shown(table, row)) {
            if (row == PRINT_EDGE_ROWS) {
                printf("| %*s |", (int)num_width, "...");
                for (uint64_t col = 0; col < table->col_count; col++) {
                    printf(" %-*s |", (int)widths[col], "...");
                }
                putchar('\n');
            }
            continue;
        }
        print_row(table, num_width, widths, row);
    }

    print_border(num_width, widths, table->col_count);
    printf("(%" PRIu64 " rows, %" PRIu64 " columns)\n", table->row_count, table->col_count);

    free(widths);
}

void free_table(TABLE *table)
{
    for (uint64_t col = 0; col < table->col_count; col++) {
        free(table->columns[col].name);
        free(table->columns[col].data);
    }

    free(table->columns);

    table->columns = NULL;
    table->col_count = 0;
    table->row_count = 0;
}

BdbStatus bdb_find_column(const TABLE *table, const char *name, size_t *col_idx, BdbError *err) {
    for (size_t col = 0; col < table->col_count; col++) {
        if (strcmp(table->columns[col].name, name) == 0) {
            *col_idx = col;
            return BDB_OK;
        }
    }

    return bdb_error_set(err, BDB_ERR_NOT_FOUND, "%s", "The column was not found");
}