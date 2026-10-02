cmake_minimum_required(VERSION 3.21)
project(BulletDB C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(BDB_BUILD_TESTS "Build the test suite" ON)
option(BDB_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang on Linux/macOS; not MinGW)" OFF)

# Sanitizers apply to every target, so they're set before any target is defined.
if (BDB_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif ()

# Warnings that catch real bugs. A missing `return` in a non-void function is
# an error, because it caused several crashes before this was added.
function(bdb_warnings target)
    if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
                -Wall -Wextra
                -Werror=return-type
                -Werror=implicit-function-declaration)
    endif ()
endfunction()

# The engine, as a library: the program and the tests both link it.
add_library(bulletdb STATIC
        src/common/common.c
        src/core/chunk.c
        src/core/table.c
        src/csv/csv_reader.c
        src/csv/csv_tokenize.c
        src/storage/io.c
        src/storage/memory.c
        src/storage/bdb_writer.c
        src/storage/bdb_reader.c
        src/storage/debug.c
        src/executor/scan.c
        src/executor/aggregate.c
        src/executor/explain.c
        src/executor/filter.h
        src/executor/filter.c
        src/csv/csv_sniffer.h
        src/csv/csv_sniffer.c
)
# Include paths are relative to src/, e.g. #include "core/chunk.h"
target_include_directories(bulletdb PUBLIC src)
bdb_warnings(bulletdb)

add_executable(BulletDB main.c
        src/logical/bdb_field.h
        src/logical/bdb_schema.h
        src/logical/bdb_logical_plan.h
        src/logical/bdb_expr.h
        src/logical/bdb_expr.c)
target_link_libraries(BulletDB PRIVATE bulletdb)
bdb_warnings(BulletDB)

if (BDB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()
