//
// test_corrupt: damaged or wrong files are rejected with an error, never a
// crash, and bdb_reader_close is safe after a failed open.
//
// Starts from a small valid file and breaks it in one way at a time.
//
// Not tested yet, because bdb_reader_open doesn't validate them (see the
// README's Known bugs): a col_count outside 1..100, group row counts that
// don't add up to row_count, and a footer that doesn't end at the trailer.
// Add them here when that validation is added.
//

#include "test_util.h"

#define TRAILER_SIZE 12      // u64 footer_offset + "BDB1"

// Opens `path`, expects `expected`, then closes (which must be safe either way).
static void expect_open(const char *label, const char *path, BdbStatus expected) {
    BDB_READER reader = {0};
    BdbError err = {0};
    BdbStatus status = bdb_reader_open(&reader, path, &err);
    if (status != expected) {
        fprintf(stderr, "%s: bdb_reader_open returned %d (%s: %s), expected %d (%s)\n",
                label, (int)status, bdb_status_str(status), err.message,
                (int)expected, bdb_status_str(expected));
        test_failures++;
    }
    bdb_reader_close(&reader);
}

// Writes a modified copy of the valid file, then expects it to be rejected.
static void expect_rejected(const char *label, const unsigned char *data, size_t size) {
    if (!write_bytes("corrupt.bdb", data, size)) {
        fprintf(stderr, "%s: could not write the test file\n", label);
        test_failures++;
        return;
    }
    expect_open(label, "corrupt.bdb", BDB_ERR_FORMAT);
}

int main(void) {
    BdbError err = {0};

    REQUIRE(write_text_file("corrupt.csv", "x,y\n1,2.5\n3,4.5\n"));
    CHECK_STATUS(csv_to_bdb("corrupt.csv", "valid.bdb", &err), BDB_OK, err);
    REQUIRE(test_failures == 0);

    size_t size = 0;
    unsigned char *valid = read_file("valid.bdb", &size);
    REQUIRE(valid != NULL && size > 20);
    unsigned char *copy = malloc(size);
    REQUIRE(copy != NULL);

    // The untouched file opens.
    expect_open("valid file", "valid.bdb", BDB_OK);

    // A file that doesn't exist.
    expect_open("missing file", "does-not-exist.bdb", BDB_ERR_OPEN);

    // Too small to hold a header and a trailer.
    expect_rejected("empty file", valid, 0);
    expect_rejected("10-byte file", valid, 10);

    // Wrong magic at the start.
    memcpy(copy, valid, size);
    copy[0] = 'X';
    expect_rejected("bad header magic", copy, size);

    // A version this reader doesn't know.
    memcpy(copy, valid, size);
    copy[4] = 2;
    expect_rejected("unknown version", copy, size);

    // Wrong magic at the end.
    memcpy(copy, valid, size);
    copy[size - 1] = 'X';
    expect_rejected("bad trailer magic", copy, size);

    // Cut off: the last 5 bytes are missing, so the trailer is in the wrong place.
    expect_rejected("truncated file", valid, size - 5);

    // footer_offset points past the end of the file.
    memcpy(copy, valid, size);
    uint64_t past_end = (uint64_t)size + 1000;
    memcpy(copy + size - TRAILER_SIZE, &past_end, sizeof past_end);
    expect_rejected("footer offset past the end", copy, size);

    // A CSV file is not a .bdb file.
    expect_open("CSV passed as .bdb", "corrupt.csv", BDB_ERR_FORMAT);

    free(copy);
    free(valid);
    return test_report("test_corrupt");
}
