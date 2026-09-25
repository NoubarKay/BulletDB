cmake_minimum_required(VERSION 4.3)
project(BulletDB)

set(CMAKE_CXX_STANDARD 20)

add_executable(BulletDB main.c
        src/engine/common.h
        src/engine/table.h
        src/engine/table.c
        src/csv.h
        src/csv.c
        src/engine/common.c
        src/engine/storage/bdb.h
        src/engine/storage/bdb.c
        src/engine/storage/format.h
        src/engine/storage/io.h
        src/engine/storage/io.c
        src/engine/storage/bdb_reader.c
        src/engine/query/query.h
        src/engine/query/query.c)
