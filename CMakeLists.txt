cmake_minimum_required(VERSION 4.3)
project(BulletDB C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(BulletDB
        main.c
        src/common/common.c
        src/core/chunk.c
        src/core/table.c
        src/csv/csv_reader.c
        src/csv/csv_tokenize.c
        src/query/query.c
        src/storage/io.c
        src/storage/bdb_writer.h
        src/storage/bdb_writer.c
        src/storage/memory.h
        src/storage/memory.c
        src/storage/debug.c
        src/storage/bdb_reader.h
        src/storage/bdb_reader.c
        src/executor/bdb_operator.h
        src/executor/scan.h
        src/executor/scan.c
        src/executor/aggregate.h
        src/executor/aggregate.c
)

# Include paths are relative to src/, e.g. #include "core/chunk.h"
target_include_directories(BulletDB PRIVATE src)

add_executable(test test.c)
