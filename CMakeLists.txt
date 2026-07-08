#[[cmake_minimum_required(VERSION 3.20)

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
# Helper: apply LTO
# =========================================================
function(kvdb_apply_lto target_name)
    if(KVDB_ENABLE_LTO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT lto_supported OUTPUT lto_error)

        if(lto_supported)
            set_property(TARGET ${target_name} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        else()
            message(WARNING "LTO not supported: ${lto_error}")
        endif()
    endif()
endfunction()

# =========================================================
# Helper: warnings
# =========================================================
function(kvdb_apply_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4 /WX /permissive- /EHsc
            /Zc:__cplusplus /Zc:preprocessor /Zc:inline
            /utf-8
        )
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall -Wextra -Wpedantic -Werror
            -Wconversion -Wsign-conversion -Wshadow
            -Wnon-virtual-dtor -Wold-style-cast
            -Wcast-align -Wunused -Woverloaded-virtual
            -Wnull-dereference -Wdouble-promotion -Wformat=2
            -Wimplicit-fallthrough -Wmisleading-indentation
            -Wduplicated-cond -Wduplicated-branches -Wlogical-op
        )
    endif()
endfunction()

# =========================================================
# Helper: sanitizers
# =========================================================
function(kvdb_apply_sanitizers target_name)
    if(KVDB_ENABLE_SANITIZERS)
        if(MSVC)
            target_compile_options(${target_name} PRIVATE
                /fsanitize=address /Zi
            )
            target_link_options(${target_name} PRIVATE
                /INCREMENTAL:NO
            )
        else()
            target_compile_options(${target_name} PRIVATE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer -g
            )
            target_link_options(${target_name} PRIVATE
                -fsanitize=address,undefined
            )
        endif()
    endif()
endfunction()

# =========================================================
# Core library
# =========================================================
find_package(Threads REQUIRED)

add_library(kvdb_core
    src/red_black_tree.cpp
    src/mem_table.cpp
    src/wal.cpp
    src/sstable.cpp
    src/engine.cpp
    src/arena.cpp
)

target_include_directories(kvdb_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
find_package(Threads REQUIRED)

# Try system zlib first
find_package(ZLIB QUIET)

if(ZLIB_FOUND)
    message(STATUS "Using system zlib")
    set(KVDB_ZLIB_TARGET ZLIB::ZLIB)
else()
    message(STATUS "Using bundled zlib")

    # Optional but nice: don't build zlib examples/tests
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "Disable zlib examples" FORCE)

    add_subdirectory(third_party/zlib EXCLUDE_FROM_ALL)

    if(TARGET zlibstatic)
        set(KVDB_ZLIB_TARGET zlibstatic)
    elseif(TARGET zlib)
        set(KVDB_ZLIB_TARGET zlib)
    else()
        message(FATAL_ERROR "Bundled zlib target not found")
    endif()
endif()


target_link_libraries(kvdb_core PRIVATE
    ${KVDB_ZLIB_TARGET}
    Threads::Threads
)
target_include_directories(kvdb_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib
)
kvdb_apply_warnings(kvdb_core)
kvdb_apply_sanitizers(kvdb_core)

# =========================================================
# Main application
# =========================================================
add_executable(kvdb_app src/main.cpp)

target_link_libraries(kvdb_app PRIVATE kvdb_core)

kvdb_apply_warnings(kvdb_app)
kvdb_apply_sanitizers(kvdb_app)

# =========================================================
# Tests
# =========================================================
if(KVDB_ENABLE_TESTS)
    enable_testing()

    include(FetchContent)

    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
    )

    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(googletest)

    include(GoogleTest)

    function(kvdb_add_test test_name)
        add_executable(${test_name} ${ARGN})

        target_link_libraries(${test_name} PRIVATE
            kvdb_core
            GTest::gtest_main
            Threads::Threads
        )

        kvdb_apply_warnings(${test_name})
        kvdb_apply_sanitizers(${test_name})

        gtest_discover_tests(${test_name})
    endfunction()

    kvdb_add_test(arena_test tests/arena_test.cpp)
endif()

# =========================================================
# Benchmarks
# =========================================================
if(KVDB_ENABLE_BENCHMARKS)
    function(kvdb_add_benchmark name)
        add_executable(${name} ${ARGN})

        target_link_libraries(${name} PRIVATE kvdb_core)

        kvdb_apply_warnings(${name})
    endfunction()

    kvdb_add_benchmark(mem_table_benchmark benchmarks/mem_table_benchmark.cpp)
    kvdb_add_benchmark(wal_benchmark benchmarks/wal_benchmark.cpp)
    kvdb_add_benchmark(sstable_benchmark benchmarks/sstable_benchmark.cpp)
    kvdb_add_benchmark(engine_benchmark benchmarks/engine_benchmark.cpp)
endif()

# =========================================================
# LTO
# =========================================================
if(KVDB_ENABLE_LTO)
    kvdb_apply_lto(kvdb_core)
    kvdb_apply_lto(kvdb_app)
endif()

# =========================================================
# Install (optional but strong signal)
# =========================================================
install(TARGETS kvdb_core kvdb_app
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)#]]
cmake_minimum_required(VERSION 3.20)

project(kvdb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KVDB_ENABLE_SANITIZERS "Enable AddressSanitizer" OFF)

if (MSVC AND KVDB_ENABLE_SANITIZERS)
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/INCREMENTAL:NO)
endif()

include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

#find_package(ZLIB REQUIRED)

# =========================
# Libraries
# =========================

### Status

add_library(kvdb_status
    "src/status.cpp"
)

target_include_directories(kvdb_status PUBLIC 
    include
)

### Arena

add_library(kvdb_arena
    src/arena.cpp
)

target_include_directories(kvdb_arena PUBLIC
    include
)

target_link_libraries(kvdb_arena PUBLIC
    kvdb_status
)

### Endian IO

add_library(kvdb_endian_io
    src/endian_io.cpp
)

target_include_directories(kvdb_endian_io PUBLIC
    include
)


target_link_libraries(kvdb_endian_io PUBLIC
    kvdb_status
)

### File

add_library(kvdb_file
    src/file.cpp
)

target_include_directories(kvdb_file PUBLIC
    include
)


target_link_libraries(kvdb_file PUBLIC
    kvdb_status
)


### File helpers

add_library(kvdb_file_helpers
    src/file_helpers.cpp
)

target_include_directories(kvdb_file_helpers PUBLIC
    include
)


target_link_libraries(kvdb_file_helpers PUBLIC
    kvdb_status
)


### CRC32 helpers

add_library(kvdb_crc32_helpers
    src/crc32_helpers.cpp
)

target_include_directories(kvdb_crc32_helpers PUBLIC
    include
)

target_link_libraries(kvdb_crc32_helpers PUBLIC
    kvdb_endian_io
    #ZLIB::ZLIB
)


target_link_libraries(kvdb_crc32_helpers PUBLIC
    kvdb_status
)


### Record

add_library(kvdb_record
    src/record.cpp
)

target_include_directories(kvdb_record PUBLIC
    include
)

target_link_libraries(kvdb_record PUBLIC
    kvdb_arena
    kvdb_endian_io
)

target_link_libraries(kvdb_record PUBLIC
    kvdb_status
)



### Red-Black Tree

add_library(kvdb_red_black_tree
    src/red_black_tree.cpp
)

target_include_directories(kvdb_red_black_tree PUBLIC
    include
)

target_link_libraries(kvdb_red_black_tree PUBLIC
    kvdb_arena
    kvdb_record
)


target_link_libraries(kvdb_red_black_tree PUBLIC
    kvdb_status
)

### SSTable Entities

### File header section

add_library(kvdb_file_header_section
    src/sstable_entities/file_header_section.cpp
)

target_include_directories(kvdb_file_header_section PUBLIC
    include
)

target_link_libraries(kvdb_file_header_section PUBLIC
    kvdb_file_helpers
    kvdb_file
    kvdb_status
    kvdb_endian_io
    kvdb_crc32_helpers
)

### Data section

add_library(kvdb_data_section
    src/sstable_entities/data_section.cpp
)

target_include_directories(kvdb_data_section PUBLIC
    include
)

target_link_libraries(kvdb_data_section PUBLIC
    kvdb_file_helpers
    kvdb_file
    kvdb_status
    kvdb_endian_io
    kvdb_crc32_helpers
)

### Data section view

add_library(kvdb_data_section_view
    src/sstable_entities/data_section_view.cpp
)

target_include_directories(kvdb_data_section_view PUBLIC
    include
)

target_link_libraries(kvdb_data_section_view PUBLIC
    kvdb_file_helpers
    kvdb_file
    kvdb_status
    kvdb_endian_io
    kvdb_crc32_helpers
)

### Index section

add_library(kvdb_index_section
    src/sstable_entities/index_section.cpp
)

target_include_directories(kvdb_index_section PUBLIC
    include
)

target_link_libraries(kvdb_index_section PUBLIC
    kvdb_file_helpers
    kvdb_file
    kvdb_status
    kvdb_endian_io
    kvdb_crc32_helpers
)

### Bloom section

add_library(kvdb_bloom_section
    src/sstable_entities/bloom_section.cpp
)

target_include_directories(kvdb_bloom_section PUBLIC
    include
)

target_link_libraries(kvdb_bloom_section PUBLIC
    kvdb_file_helpers
    kvdb_file
    kvdb_status
    kvdb_endian_io
    kvdb_crc32_helpers
)

### Meta section

add_library(kvdb_meta_section
    src/sstable_entities/meta_section.cpp
)

target_include_directories(kvdb_meta_section PUBLIC
    include
)

target_link_libraries(kvdb_meta_section PUBLIC
    kvdb_file_helpers
    kvdb_file
    kvdb_status
    kvdb_endian_io
    kvdb_crc32_helpers
)

### File footer section

add_library(kvdb_file_footer_section
    src/sstable_entities/file_footer_section.cpp
)

target_include_directories(kvdb_file_footer_section PUBLIC
    include
)

target_link_libraries(kvdb_file_footer_section PUBLIC
    kvdb_file_helpers
    kvdb_file
    kvdb_status
    kvdb_endian_io
    kvdb_crc32_helpers
)


### SSTable

add_library(kvdb_sstable
    src/sstable.cpp
)

target_include_directories(kvdb_sstable PUBLIC
    include
)

target_link_libraries(kvdb_sstable PUBLIC
    kvdb_crc32_helpers
    kvdb_file_helpers
    kvdb_arena
    kvdb_red_black_tree
    kvdb_endian_io
    kvdb_record
    kvdb_status
    kvdb_file_header_section
    kvdb_data_section
    kvdb_data_section_view
    kvdb_index_section
    kvdb_bloom_section
    kvdb_meta_section
    kvdb_file_footer_section
)

### SStable writer

add_library(kvdb_sstable_writer 
    src/sstable_writer.cpp
)

target_include_directories(kvdb_sstable_writer PUBLIC
    include
)

target_link_libraries(kvdb_sstable_writer PUBLIC
    kvdb_status
    kvdb_arena
    kvdb_sstable
    kvdb_file
    kvdb_file_helpers
)

### SSTable loader

add_library(kvdb_sstable_loader
    src/sstable_loader.cpp
)

target_include_directories(kvdb_sstable_loader PUBLIC
    include
)

target_link_libraries(kvdb_sstable_loader PUBLIC
    kvdb_status
    kvdb_arena
    kvdb_sstable
    kvdb_file
    kvdb_file_helpers
)

### SSTable manager

add_library(kvdb_sstable_manager
    src/sstable_manager.cpp
)

target_include_directories(kvdb_sstable_manager PUBLIC
    include
)

target_link_libraries(kvdb_sstable_manager PUBLIC
    kvdb_status
    kvdb_arena
    kvdb_sstable
    kvdb_file
    kvdb_file_helpers
    kvdb_sstable_loader
    kvdb_sstable_writer
)

### Table Meta

add_library(kvdb_table_meta 
    src/table_meta.cpp
)

target_include_directories(kvdb_table_meta PUBLIC 
    include
)

target_link_libraries(kvdb_table_meta PUBLIC 
    kvdb_arena
    kvdb_status
    kvdb_sstable
)

### Level Manager

add_library(kvdb_level_manager
    src/level_manager.cpp
)

target_include_directories(kvdb_level_manager PUBLIC 
    include
)

target_link_libraries(kvdb_level_manager PUBLIC
    kvdb_arena
    kvdb_status
    kvdb_table_meta
)


### Merge iterator

add_library(kvdb_merge_iterator
    src/merge_iterator.cpp
)

target_include_directories(kvdb_merge_iterator PUBLIC
    include
)

target_link_libraries(kvdb_merge_iterator PUBLIC 
    kvdb_status
    kvdb_arena
    kvdb_table_meta
    kvdb_sstable
    kvdb_record
)

### Manifest

add_library(kvdb_manifest 
    src/manifest.cpp
)

target_include_directories(kvdb_manifest PUBLIC
    include
)

target_link_libraries(kvdb_manifest PUBLIC
     kvdb_level_manager
     kvdb_table_meta
     kvdb_file
     kvdb_file_helpers
     kvdb_arena
)

### WAL

add_library(kvdb_wal
    src/wal.cpp
)

target_include_directories(kvdb_wal PUBLIC
    include
)

target_link_libraries(kvdb_wal PUBLIC
    kvdb_arena
    kvdb_file
    kvdb_file_helpers
    kvdb_status
    kvdb_sstable
)

### MemTable

add_library(kvdb_mem_table
    src/mem_table.cpp
)

target_include_directories(kvdb_mem_table PUBLIC 
    include
)

target_link_libraries(kvdb_mem_table PUBLIC
    kvdb_wal
    kvdb_file
    kvdb_file_helpers
    kvdb_arena
    kvdb_wal
)

### engine

add_library(kvdb_engine
    src/engine.cpp
)

target_include_directories(kvdb_engine PUBLIC
    include
)

target_link_libraries(kvdb_engine PUBLIC
    kvdb_sstable
    kvdb_manifest
    kvdb_wal
    kvdb_mem_table
    kvdb_status
)

### Compaction entities

### Compaction plan

add_library(kvdb_compaction_plan
    src/compaction_plan.cpp
)

target_include_directories(kvdb_compaction_plan PUBLIC
    include
)

target_link_libraries(kvdb_compaction_plan PUBLIC
    kvdb_status
    kvdb_arena
    kvdb_table_meta
)

### Compaction job

add_library(kvdb_compaction_job
    src/compaction_job.cpp
)

target_include_directories(kvdb_compaction_job PUBLIC
    include
)

target_link_libraries(kvdb_compaction_job PUBLIC
    kvdb_arena
    kvdb_status
    kvdb_compaction_plan
    kvdb_table_meta
)

### Compaction scheduler

add_library(kvdb_compaction_scheduler 
    src/compaction_scheduler.cpp
)

target_include_directories(kvdb_compaction_scheduler PUBLIC
    include
)

target_link_libraries(kvdb_compaction_scheduler PUBLIC
    kvdb_arena
    kvdb_status
    kvdb_compaction_job
    kvdb_compaction_plan
    kvdb_table_meta
)

### SStable Builder

add_library(kvdb_sstable_builder
    src/sstable_builder.cpp
)

target_link_directories(kvdb_sstable_builder PUBLIC
    include
)

target_link_libraries(kvdb_sstable_builder PUBLIC
    kvdb_arena
    kvdb_status
    kvdb_sstable
    kvdb_file
    kvdb_file_helpers
    kvdb_sstable_iterator
    kvdb_sstable
)

# =========================
# Tests
# =========================

enable_testing()

include(GoogleTest)

### File

add_executable(kvdb_file_test
    tests/file_test.cpp
)

target_link_libraries(kvdb_file_test PRIVATE
    kvdb_status
    kvdb_file
    GTest::gtest_main
)

gtest_discover_tests(kvdb_file_test)

### Arena

add_executable(kvdb_arena_test
    tests/arena_test.cpp
)

target_link_libraries(kvdb_arena_test PRIVATE
    kvdb_arena
    GTest::gtest_main
)

gtest_discover_tests(kvdb_arena_test)

### Endian IO

add_executable(kvdb_endian_io_test
    tests/endian_io_test.cpp
)

target_link_libraries(kvdb_endian_io_test PRIVATE
    kvdb_endian_io
    kvdb_arena
    kvdb_record
    GTest::gtest_main
)

gtest_discover_tests(kvdb_endian_io_test)

### Record

add_executable(kvdb_record_test
    tests/record_test.cpp
)

target_link_libraries(kvdb_record_test PRIVATE
    kvdb_record
    GTest::gtest_main
)

gtest_discover_tests(kvdb_record_test)

### Red-Black Tree

add_executable(kvdb_red_black_tree_test
    tests/red_black_tree_test.cpp
)

target_link_libraries(kvdb_red_black_tree_test PRIVATE
    kvdb_red_black_tree
    GTest::gtest_main
)

gtest_discover_tests(kvdb_red_black_tree_test)

### CRC32 helpers

add_executable(kvdb_crc32_helpers_test
    tests/crc32_helpers_test.cpp
)

target_link_libraries(kvdb_crc32_helpers_test PRIVATE
    kvdb_crc32_helpers
    GTest::gtest_main
)

gtest_discover_tests(kvdb_crc32_helpers_test)

### SSTable

add_executable(kvdb_sstable_test
    tests/sstable_test.cpp
)

target_link_libraries(kvdb_sstable_test PRIVATE
    kvdb_sstable
    GTest::gtest_main
)

gtest_discover_tests(kvdb_sstable_test)
