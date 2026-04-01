cmake_minimum_required(VERSION 3.20)

project(KVDB VERSION 0.1.0 LANGUAGES CXX)

# =========================================================
# Global C++ settings
# =========================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# =========================================================
# Project options
# =========================================================
option(KVDB_ENABLE_TESTS "Build tests" OFF)
option(KVDB_ENABLE_SANITIZERS "Enable sanitizers" OFF)
option(KVDB_ENABLE_BENCHMARKS "Build benchmarks" OFF)
option(KVDB_ENABLE_LTO "Enable LTO" OFF)

# =========================================================
# Helper: apply LTO optimization
# =========================================================
function(kvdb_apply_lto target_name)
    if(KVDB_ENABLE_LTO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT lto_supported OUTPUT lto_error)

        if(lto_supported)
            set_property(TARGET ${target_name} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        else()
            message(WARNING "LTO/IPO not supported: ${lto_error}")
        endif()
    endif()
endfunction()

# =========================================================
# Helper: apply compiler warnings
# =========================================================
function(kvdb_apply_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4
            /WX
            /permissive-
            /EHsc
            /Zc:__cplusplus
            /Zc:preprocessor
            /Zc:inline
            /utf-8
        )
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror

            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Wunused
            -Woverloaded-virtual
            -Wnull-dereference
            -Wdouble-promotion
            -Wformat=2

            -Wimplicit-fallthrough
            -Wmisleading-indentation
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
        )
    endif()
endfunction()

# =========================================================
# Helper: apply sanitizers
# =========================================================
function(kvdb_apply_sanitizers target_name)
    if(KVDB_ENABLE_SANITIZERS)
        if(MSVC)
            target_compile_options(${target_name} PRIVATE
                /fsanitize=address
                /Zi
            )
            target_link_options(${target_name} PRIVATE
                /INCREMENTAL:NO
            )
        else()
            target_compile_options(${target_name} PRIVATE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
                -g
            )
            target_link_options(${target_name} PRIVATE
                -fsanitize=address,undefined
            )
        endif()
    endif()
endfunction()

# =========================================================
# Helper: add a test target
# =========================================================
function(kvdb_add_test test_name)
    add_executable(${test_name} ${ARGN})

    target_link_libraries(${test_name} PRIVATE
        kvdb_core
    )

    kvdb_apply_warnings(${test_name})
    kvdb_apply_sanitizers(${test_name})

    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

# =========================================================
# Helper: add a benchmark target
# =========================================================
function(kvdb_add_benchmark benchmark_name)
    add_executable(${benchmark_name} ${ARGN})

    target_link_libraries(${benchmark_name} PRIVATE
        kvdb_core
    )

    kvdb_apply_warnings(${benchmark_name})

    # Usually DO NOT apply sanitizers to benchmarks
    # because they distort performance measurements.
endfunction()

# =========================================================
# Third-party dependencies
# =========================================================
add_subdirectory(third_party/zlib)

# =========================================================
# Core library
# =========================================================
add_library(kvdb_core
    src/red_black_tree.cpp
    src/mem_table.cpp
    src/wal.cpp
    src/sstable.cpp
    src/engine.cpp
    src/encoding.cpp
)

target_include_directories(kvdb_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(kvdb_core PRIVATE
    zlibstatic
)

kvdb_apply_warnings(kvdb_core)
kvdb_apply_sanitizers(kvdb_core)

# =========================================================
# Main application
# =========================================================
add_executable(kvdb_app
    src/main.cpp
)

target_link_libraries(kvdb_app PRIVATE
    kvdb_core
)

kvdb_apply_warnings(kvdb_app)
kvdb_apply_sanitizers(kvdb_app)

# =========================================================
# Tests
# =========================================================
if(KVDB_ENABLE_TESTS)
    enable_testing()

    kvdb_add_test(red_black_tree_test tests/red_black_tree_test.cpp)
    kvdb_add_test(mem_table_test     tests/mem_table_test.cpp)
    kvdb_add_test(wal_test           tests/wal_test.cpp)
    kvdb_add_test(sstable_test       tests/sstable_test.cpp)
    kvdb_add_test(engine_test        tests/engine_test.cpp)
endif()

# =========================================================
# Benchmarks
# =========================================================
if(KVDB_ENABLE_BENCHMARKS)
    kvdb_add_benchmark(mem_table_benchmark benchmarks/mem_table_benchmark.cpp)
    kvdb_add_benchmark(wal_benchmark       benchmarks/wal_benchmark.cpp)
    kvdb_add_benchmark(sstable_benchmark   benchmarks/sstable_benchmark.cpp)
    kvdb_add_benchmark(engine_benchmark    benchmarks/engine_benchmark.cpp)
endif()

# =========================================================
# LTO/IPO
# =========================================================
if(KVDB_ENABLE_LTO)
    kvdb_apply_lto(kvdb_core)
    kvdb_apply_lto(kvdb_app)
endif()