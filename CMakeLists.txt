cmake_minimum_required(VERSION 3.13)
project(MiniRocketDB)

# AddressSanitizer is opt-in so the default static library remains suitable
# for normal consumers, including cgo callers.
option(ROCKETDB_ENABLE_ASAN "Build RocketDB with AddressSanitizer (debug only)" OFF)
option(ROCKETDB_BUILD_BENCHMARKS "Build RocketDB benchmark tools" OFF)
set(ROCKETDB_LEVELDB_SOURCE_DIR "" CACHE PATH "Optional LevelDB source tree for shared-driver comparisons")

# Keep the production library separate from command-line tools.  In
# particular, src/db/levelbuild.cc defines its own main() and must not become
# part of librocketdb.a.
set(ROCKETDB_SOURCES
    src/db/builder.cc
    src/db/c.cc
    src/db/db_impl.cc
    src/db/db_iter.cc
    src/db/dbformat.cc
    src/db/dumpfile.cc
    src/db/filename.cc
    src/db/memtable.cc
    src/db/repair.cc
    src/db/table_cache.cc
    src/db/version_edit.cc
    src/db/version_set.cc
    src/db/write_batch.cc
    src/table/block.cc
    src/table/block_builder.cc
    src/table/filter_block.cc
    src/table/format.cc
    src/table/iterator.cc
    src/table/merger.cc
    src/table/table.cc
    src/table/table_builder.cc
    src/table/two_level_iterator.cc
    src/util/arena.cc
    src/util/bloom.cc
    src/util/cache.cc
    src/util/coding.cc
    src/util/comparator.cc
    src/util/crc32c.cc
    src/util/env.cc
    src/util/env_posix.cc
    src/util/filter_policy.cc
    src/util/hash.cc
    src/util/histogram.cc
    src/util/logging.cc
    src/util/options.cc
    src/util/status.cc
    src/wal/log_reader.cc
    src/wal/log_writer.cc
)

add_library(rocketdb STATIC ${ROCKETDB_SOURCES})
target_compile_features(rocketdb PUBLIC cxx_std_17)
target_include_directories(rocketdb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

find_package(Threads REQUIRED)
target_link_libraries(rocketdb PUBLIC Threads::Threads)

if(ROCKETDB_ENABLE_ASAN)
    message(STATUS "RocketDB: AddressSanitizer ENABLED (debug build)")
    target_compile_options(rocketdb PRIVATE -fsanitize=address -g)
    # rocketdb is static, so consumers must link the sanitizer runtime too.
    target_link_options(rocketdb INTERFACE -fsanitize=address)
else()
    message(STATUS "RocketDB: clean release build (-O2, no sanitizer)")
    target_compile_options(rocketdb PRIVATE -O2)
endif()

if(ROCKETDB_BUILD_BENCHMARKS)
    add_executable(rocketdb_bench
        bench/db_bench.cc
    )
    target_link_libraries(rocketdb_bench PRIVATE rocketdb)
    # Histogram is an internal utility intentionally reused by db_bench.
    target_include_directories(rocketdb_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(rocketdb_bench PRIVATE
        ROCKETDB_BENCH_RESULT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/bench/basic_benchmark_results.md"
    )

    add_executable(rocketdb_mixed_bench
        bench/mixed_bench.cc
    )
    target_link_libraries(rocketdb_mixed_bench PRIVATE rocketdb)
    target_include_directories(rocketdb_mixed_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(rocketdb_mixed_bench PRIVATE
        ROCKETDB_MIXED_BENCH_RESULT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/bench/mixed_benchmark_results.md"
    )

    if(ROCKETDB_ENABLE_ASAN)
        target_compile_options(rocketdb_bench PRIVATE -fsanitize=address -g)
        target_compile_options(rocketdb_mixed_bench PRIVATE -fsanitize=address -g)
    endif()

    if(ROCKETDB_LEVELDB_SOURCE_DIR)
        if(ROCKETDB_ENABLE_ASAN)
            message(FATAL_ERROR "Use a non-ASan build for engine performance comparisons")
        endif()
        set(LEVELDB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
        set(LEVELDB_INSTALL OFF CACHE BOOL "" FORCE)
        add_subdirectory("${ROCKETDB_LEVELDB_SOURCE_DIR}"
                         "${CMAKE_BINARY_DIR}/comparison-leveldb" EXCLUDE_FROM_ALL)

        # Both engines run the same workload, key generation, timers, latency
        # sampling, and histogram implementation in separate executables.
        add_executable(leveldb_bench bench/db_bench.cc src/util/histogram.cc)
        target_link_libraries(leveldb_bench PRIVATE leveldb)
        target_compile_features(leveldb_bench PRIVATE cxx_std_17)
        target_compile_definitions(leveldb_bench PRIVATE
            ROCKETDB_BENCH_USE_LEVELDB=1
            ROCKETDB_BENCH_RESULT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/bench/leveldb_basic_benchmark_results.md"
        )

        add_executable(leveldb_mixed_bench bench/mixed_bench.cc)
        target_link_libraries(leveldb_mixed_bench PRIVATE leveldb)
        target_compile_features(leveldb_mixed_bench PRIVATE cxx_std_17)
        target_include_directories(leveldb_mixed_bench PRIVATE
            "${ROCKETDB_LEVELDB_SOURCE_DIR}"
            "${CMAKE_BINARY_DIR}/comparison-leveldb/include"
        )
        target_compile_definitions(leveldb_mixed_bench PRIVATE
            ROCKETDB_BENCH_USE_LEVELDB=1
            ROCKETDB_MIXED_BENCH_RESULT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/bench/leveldb_mixed_benchmark_results.md"
        )
        # LevelDB's CMake build disables RTTI; its benchmark-side subclasses
        # must use the same convention. Exceptions remain enabled in drivers.
        if(MSVC)
            target_compile_options(leveldb_bench PRIVATE /GR-)
            target_compile_options(leveldb_mixed_bench PRIVATE /GR-)
        else()
            target_compile_options(rocketdb PRIVATE -fno-exceptions)
            target_compile_options(leveldb_bench PRIVATE -fno-rtti)
            target_compile_options(leveldb_mixed_bench PRIVATE -fno-rtti)
        endif()
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(rocketdb_regression_test
        test/db_regression_test.cc
    )
    target_link_libraries(rocketdb_regression_test PRIVATE rocketdb)
    target_include_directories(rocketdb_regression_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    if(ROCKETDB_ENABLE_ASAN)
        target_compile_options(rocketdb_regression_test PRIVATE -fsanitize=address -g)
    endif()

    add_test(NAME rocketdb_regression COMMAND rocketdb_regression_test)
    set_tests_properties(rocketdb_regression PROPERTIES TIMEOUT 60)
endif()
