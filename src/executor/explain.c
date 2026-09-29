#include "bdb_operator.h"

void bdb_explain(BdbOperator *op, FILE *out) {
    size_t depth = 0;
    while (op != NULL) {
        fprintf(out, "%*s", (int)(depth * 8), "");
        if (depth > 0) {
            fprintf(out, "+-");
        }
        op->describe(op, out);
        op = op->child;
        depth++;
    }
}
