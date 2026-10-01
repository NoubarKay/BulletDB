//
// test_format: the .bdb v1 layout, byte for byte.
//
// Writes the one-row CSV from the worked example in docs/DESIGN.md and checks
// that the file is exactly the 193 bytes verified by hand. Any change to the
// file layout (for example format v2 with per-group statistics) must update
// both this test and the worked example.
//

#include "test_util.h"

static const unsigned char EXPECTED[193] = {
    // header: "BDB1", version 1
    0x42, 0x44, 0x42, 0x31, 0x01, 0x00, 0x00, 0x00,
    // row group 0 (1 row): each column is [bitmap byte][8-byte value]
    0x01, 0x7B, 0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // ORDERNUMBER     10107
    0x01, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // QUANTITYORDERED 30
    0x01, 0xCD, 0xCC, 0xCC, 0xCC, 0xCC, 0xEC, 0x57, 0x40,   // PRICEEACH       95.7
    0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // ORDERLINENUMBER 2
    0x01, 0x37, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // SALES           2871
    // footer (starts at 53): row_count = 1, col_count = 5
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00,
    // columns: name_len (u16), name, type (u8)
    0x0B, 0x00, 'O','R','D','E','R','N','U','M','B','E','R', 0x00,
    0x0F, 0x00, 'Q','U','A','N','T','I','T','Y','O','R','D','E','R','E','D', 0x00,
    0x09, 0x00, 'P','R','I','C','E','E','A','C','H', 0x01,
    0x0F, 0x00, 'O','R','D','E','R','L','I','N','E','N','U','M','B','E','R', 0x00,
    0x05, 0x00, 'S','A','L','E','S', 0x00,
    // group_count = 1; group 0: row_count = 1, then 5 offsets
    0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x1A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x23, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // trailer: footer_offset = 53, "BDB1"
    0x35, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x42, 0x44, 0x42, 0x31,
};

int main(void) {
    BdbError err = {0};

    REQUIRE(write_text_file("format.csv",
        "ORDERNUMBER,QUANTITYORDERED,PRICEEACH,ORDERLINENUMBER,SALES\n"
        "10107,30,95.7,2,2871\n"));

    CHECK_STATUS(csv_to_bdb("format.csv", "format.bdb", &err), BDB_OK, err);

    size_t size = 0;
    unsigned char *data = read_file("format.bdb", &size);
    REQUIRE(data != NULL);

    CHECK(size == sizeof EXPECTED);
    size_t n = size < sizeof EXPECTED ? size : sizeof EXPECTED;
    for (size_t i = 0; i < n; i++) {
        if (data[i] != EXPECTED[i]) {
            fprintf(stderr, "byte %zu (0x%02zX): got 0x%02X, expected 0x%02X\n",
                    i, i, data[i], EXPECTED[i]);
            test_failures++;
            break;                      // the first difference is the useful one
        }
    }
    free(data);

    return test_report("test_format");
}
