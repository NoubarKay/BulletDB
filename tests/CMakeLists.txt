# Each test is a small program that returns 0 when every check passes.
# Run them all with:  ctest --test-dir <build dir> --output-on-failure
set(BDB_TESTS
        test_format
        test_roundtrip
        test_aggregates
        test_corrupt
        test_filter
        test_csv_types
)

foreach (name IN LISTS BDB_TESTS)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE bulletdb)
    bdb_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    # Tests write their CSV and .bdb files into the build directory.
    set_tests_properties(${name} PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            TIMEOUT 300)
endforeach ()
