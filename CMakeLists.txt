cmake_minimum_required(VERSION 3.20)

project(kvdb VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)

# ==============================================================================
# Options
# ==============================================================================

option(KVDB_ENABLE_TESTS              "Build unit tests" OFF)
option(KVDB_ENABLE_BENCHMARKS         "Build benchmarks" OFF)

option(KVDB_ENABLE_WARNINGS           "Enable compiler warnings" ON)
option(KVDB_WARNINGS_AS_ERRORS        "Treat compiler warnings as errors" OFF)

option(KVDB_ENABLE_ASAN               "Enable AddressSanitizer" OFF)
option(KVDB_ENABLE_UBSAN              "Enable UndefinedBehaviorSanitizer" OFF)
option(KVDB_ENABLE_TSAN               "Enable ThreadSanitizer" OFF)

option(KVDB_ENABLE_LTO                "Enable link-time/interprocedural optimization" OFF)

# ASan and TSan cannot be used together.
if(KVDB_ENABLE_ASAN AND KVDB_ENABLE_TSAN)
    message(FATAL_ERROR "KVDB_ENABLE_ASAN and KVDB_ENABLE_TSAN cannot be enabled together.")
endif()

# MSVC currently supports ASan here, but not UBSan/TSan.
if(MSVC AND (KVDB_ENABLE_UBSAN OR KVDB_ENABLE_TSAN))
    message(FATAL_ERROR "MSVC build: UBSan and TSan are not supported by this CMake configuration.")
endif()

if(KVDB_ENABLE_LTO AND
   (KVDB_ENABLE_ASAN OR KVDB_ENABLE_UBSAN OR KVDB_ENABLE_TSAN))
    message(WARNING "LTO and sanitizers are both enabled. This is usually not useful for diagnostic builds.")
endif()

# ==============================================================================
# Build options shared by KVDB targets only
# ==============================================================================

add_library(kvdb_build_options INTERFACE)

if(KVDB_ENABLE_WARNINGS)
    if(MSVC)
        target_compile_options(kvdb_build_options INTERFACE
            /W4
            /permissive-
            /EHsc
            /Zc:__cplusplus
            /Zc:preprocessor
            /Zc:inline
            /utf-8
        )

        if(KVDB_WARNINGS_AS_ERRORS)
            target_compile_options(kvdb_build_options INTERFACE /WX)
        endif()

    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(kvdb_build_options INTERFACE
            -Wall
            -Wextra
            -Wpedantic
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
        )

        # These are GCC-specific (or not consistently supported by Clang).
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(kvdb_build_options INTERFACE
                -Wmisleading-indentation
                -Wduplicated-cond
                -Wduplicated-branches
                -Wlogical-op
            )
        endif()

        if(KVDB_WARNINGS_AS_ERRORS)
            target_compile_options(kvdb_build_options INTERFACE -Werror)
        endif()
    else()
        message(WARNING
            "KVDB_ENABLE_WARNINGS is ON, but no warning profile is configured "
            "for compiler '${CMAKE_CXX_COMPILER_ID}'."
        )
    endif()
endif()

# Sanitizers.
if(MSVC)
    if(KVDB_ENABLE_ASAN)
        target_compile_options(kvdb_build_options INTERFACE
            /fsanitize=address
            /Zi
        )
        target_link_options(kvdb_build_options INTERFACE
            /INCREMENTAL:NO
        )
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set(_kvdb_sanitizers)

    if(KVDB_ENABLE_ASAN)
        list(APPEND _kvdb_sanitizers address)
    endif()

    if(KVDB_ENABLE_UBSAN)
        list(APPEND _kvdb_sanitizers undefined)
    endif()

    if(KVDB_ENABLE_TSAN)
        list(APPEND _kvdb_sanitizers thread)
    endif()

    if(_kvdb_sanitizers)
        list(JOIN _kvdb_sanitizers "," _kvdb_sanitizer_list)

        target_compile_options(kvdb_build_options INTERFACE
            "-fsanitize=${_kvdb_sanitizer_list}"
            -fno-omit-frame-pointer
        )

        target_link_options(kvdb_build_options INTERFACE
            "-fsanitize=${_kvdb_sanitizer_list}"
        )
    endif()
elseif(KVDB_ENABLE_ASAN OR KVDB_ENABLE_UBSAN OR KVDB_ENABLE_TSAN)
    message(FATAL_ERROR
        "Sanitizers requested, but compiler '${CMAKE_CXX_COMPILER_ID}' "
        "is not supported by this CMake configuration."
    )

endif()

# ==============================================================================
# MemTable driver
# ==============================================================================

set(KVDB_DRIVER "RBTREE" CACHE STRING "MemTable driver")
set_property(CACHE KVDB_DRIVER PROPERTY STRINGS RBTREE SKIPLIST TREAP)

if(NOT KVDB_DRIVER MATCHES "^(RBTREE|SKIPLIST|TREAP)$")
    message(FATAL_ERROR
        "Invalid KVDB_DRIVER='${KVDB_DRIVER}'. "
        "Expected RBTREE, SKIPLIST OR TREAP."
    )
endif()

target_compile_definitions(kvdb_build_options INTERFACE
    KVDB_DRIVER_${KVDB_DRIVER}
)


# ==============================================================================
# LTO / IPO
# ==============================================================================

set(KVDB_LTO_SUPPORTED OFF)

if(KVDB_ENABLE_LTO)
    include(CheckIPOSupported)

    check_ipo_supported(
        RESULT KVDB_LTO_SUPPORTED
        OUTPUT KVDB_LTO_ERROR
        LANGUAGES CXX
    )

    if(NOT KVDB_LTO_SUPPORTED)
        message(WARNING "LTO/IPO is not supported: ${KVDB_LTO_ERROR}")
    endif()
endif()

function(kvdb_configure_target target_name)
    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR "kvdb_configure_target(): unknown target '${target_name}'.")
    endif()

    # PRIVATE: do not force KVDB's warning/sanitizer flags on external consumers.
    target_link_libraries("${target_name}" PRIVATE kvdb_build_options)

    if(KVDB_ENABLE_LTO AND KVDB_LTO_SUPPORTED)
        set_property(
            TARGET "${target_name}"
            PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE
        )
    endif()
endfunction()

# ==============================================================================
# Component helper
# ==============================================================================
#
# Example:
#
#   kvdb_add_component(arena
#       SOURCES
#           src/arena.cpp
#       PUBLIC_DEPS
#           kvdb_status
#   )
#
# Creates target: kvdb_arena
# Registers it for automatic test/benchmark discovery.
# ==============================================================================

function(kvdb_add_component component_name)
    set(options
        PRIVATE_ZLIB_DEP
        PUBLIC_ZLIB_DEP
    )
    set(one_value_args)
    set(multi_value_args
        SOURCES
        PUBLIC_DEPS
        PRIVATE_DEPS
        PUBLIC_INCLUDES
        PRIVATE_INCLUDES
    )

    cmake_parse_arguments(
        KVDB_COMPONENT
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
        ${ARGN}
    )

    if(KVDB_COMPONENT_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "kvdb_add_component(${component_name}): unknown arguments: "
            "${KVDB_COMPONENT_UNPARSED_ARGUMENTS}"
        )
    endif()

    if(NOT KVDB_COMPONENT_SOURCES)
        message(FATAL_ERROR
            "kvdb_add_component(${component_name}) requires at least one source."
        )
    endif()

    set(target_name "kvdb_${component_name}")

    if(TARGET "${target_name}")
        message(FATAL_ERROR "Target '${target_name}' already exists.")
    endif()

    add_library("${target_name}"
        ${KVDB_COMPONENT_SOURCES}
    )

    target_include_directories("${target_name}"
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party>
            $<INSTALL_INTERFACE:third_party>
    )

    if(KVDB_COMPONENT_PUBLIC_INCLUDES)
        target_include_directories("${target_name}" PUBLIC
            ${KVDB_COMPONENT_PUBLIC_INCLUDES}
        )
    endif()

    if(KVDB_COMPONENT_PRIVATE_INCLUDES)
        target_include_directories("${target_name}" PRIVATE
            ${KVDB_COMPONENT_PRIVATE_INCLUDES}
        )
    endif()

    if(KVDB_COMPONENT_PUBLIC_DEPS)
        target_link_libraries("${target_name}" PUBLIC
            ${KVDB_COMPONENT_PUBLIC_DEPS}
        )
    endif()

    if(KVDB_COMPONENT_PRIVATE_DEPS)
        target_link_libraries("${target_name}" PRIVATE
            ${KVDB_COMPONENT_PRIVATE_DEPS}
        )
    endif()

    if(KVDB_COMPONENT_PRIVATE_ZLIB_DEP)
        target_link_libraries("${target_name}" PRIVATE
            kvdb_zlib
        )
    endif()

    if(KVDB_COMPONENT_PUBLIC_ZLIB_DEP)
        target_link_libraries("${target_name}" PUBLIC
            kvdb_zlib
        )
    endif()

    kvdb_configure_target("${target_name}")

    set_property(GLOBAL APPEND PROPERTY KVDB_COMPONENT_TARGETS "${target_name}")
    set(CMAKE)
endfunction()

# ==============================================================================
# Dependencies
# ==============================================================================

add_library(kvdb_zlib STATIC
    third_party/zlib/adler32.c
    third_party/zlib/compress.c
    third_party/zlib/crc32.c
    third_party/zlib/deflate.c
    third_party/zlib/gzclose.c
    third_party/zlib/gzlib.c
    third_party/zlib/gzread.c
    third_party/zlib/gzwrite.c
    third_party/zlib/infback.c
    third_party/zlib/inffast.c
    third_party/zlib/inflate.c
    third_party/zlib/inftrees.c
    third_party/zlib/trees.c
    third_party/zlib/uncompr.c
    third_party/zlib/zutil.c
)

target_include_directories(kvdb_zlib PUBLIC
    "${PROJECT_SOURCE_DIR}/third_party/zlib"
)

set_target_properties(kvdb_zlib PROPERTIES
    C_STANDARD 90
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
)


# ==============================================================================
# Components
# ==============================================================================

kvdb_add_component(status
    SOURCES
        src/status.cpp
)

kvdb_add_component(db_options
    SOURCES
        src/db_options.cpp
    PUBLIC_DEPS
        kvdb_status
)

kvdb_add_component(murmur_hash
    SOURCES
        src/MurmurHash3.cpp
)

kvdb_add_component(arena
    SOURCES
        src/arena.cpp
    PUBLIC_DEPS
        kvdb_status
)

kvdb_add_component(file
    SOURCES
        src/file.cpp
    PUBLIC_DEPS
        kvdb_status
)

kvdb_add_component(endian_io
    SOURCES
        src/endian_io.cpp
    PUBLIC_DEPS
        kvdb_status
        kvdb_file
        kvdb_file_helpers
)

kvdb_add_component(file_helpers
    SOURCES
        src/file_helpers.cpp
    PUBLIC_DEPS
        kvdb_status
)

kvdb_add_component(crc32_helpers
    SOURCES
        src/crc32_helpers.cpp
    PUBLIC_DEPS
        kvdb_endian_io
        kvdb_status
    PUBLIC_ZLIB_DEP
)

kvdb_add_component(record
    SOURCES
        src/record.cpp
    PUBLIC_DEPS
        kvdb_arena
        kvdb_endian_io
        kvdb_status
)

kvdb_add_component(driver
    SOURCES
        src/driver.cpp
    PUBLIC_DEPS
        kvdb_arena
        kvdb_record
        kvdb_status
)

kvdb_add_component(red_black_tree
    SOURCES
        src/red_black_tree.cpp
    PUBLIC_DEPS
        kvdb_arena
        kvdb_record
        kvdb_status
        kvdb_driver
)

kvdb_add_component(skip_list
    SOURCES
        src/skip_list.cpp
    PUBLIC_DEPS
        kvdb_arena
        kvdb_record
        kvdb_status
        kvdb_driver
)

kvdb_add_component(treap
    SOURCES
        src/treap.cpp
    PUBLIC_DEPS
        kvdb_arena
        kvdb_record
        kvdb_status
        kvdb_driver
)

kvdb_add_component(file_header_section
    SOURCES
        src/sstable_entities/file_header_section.cpp
    PUBLIC_DEPS
        kvdb_file_helpers
        kvdb_file
        kvdb_status
        kvdb_endian_io
        kvdb_crc32_helpers
)

kvdb_add_component(index_section
    SOURCES
        src/sstable_entities/index_section.cpp
    PUBLIC_DEPS
        kvdb_file_helpers
        kvdb_file
        kvdb_status
        kvdb_endian_io
        kvdb_crc32_helpers
        kvdb_arena
)

kvdb_add_component(data_section
    SOURCES
        src/sstable_entities/data_section.cpp
    PUBLIC_DEPS
        kvdb_file_helpers
        kvdb_file
        kvdb_status
        kvdb_endian_io
        kvdb_crc32_helpers
        kvdb_index_section
)

kvdb_add_component(data_section_view
    SOURCES
        src/sstable_entities/data_section_view.cpp
    PUBLIC_DEPS
        kvdb_file_helpers
        kvdb_file
        kvdb_status
        kvdb_endian_io
        kvdb_crc32_helpers
        kvdb_data_section
)

kvdb_add_component(bloom_section
    SOURCES
        src/sstable_entities/bloom_section.cpp
    PUBLIC_DEPS
        kvdb_file_helpers
        kvdb_file
        kvdb_status
        kvdb_endian_io
        kvdb_crc32_helpers
)

kvdb_add_component(file_footer_section
    SOURCES
        src/sstable_entities/file_footer_section.cpp
    PUBLIC_DEPS
        kvdb_file_helpers
        kvdb_file
        kvdb_status
        kvdb_endian_io
        kvdb_crc32_helpers
        kvdb_index_section
        kvdb_meta_section
)

kvdb_add_component(meta_section
    SOURCES
        src/sstable_entities/meta_section.cpp
    PUBLIC_DEPS
        kvdb_file_helpers
        kvdb_file
        kvdb_status
        kvdb_endian_io
        kvdb_crc32_helpers
        kvdb_data_section
)

kvdb_add_component(sstable
    SOURCES
        src/sstable.cpp
    PUBLIC_DEPS
        kvdb_crc32_helpers
        kvdb_file_helpers
        kvdb_arena
        kvdb_red_black_tree
        kvdb_endian_io
        kvdb_record
        kvdb_status
        kvdb_murmur_hash
        kvdb_file_header_section
        kvdb_data_section
        kvdb_data_section_view
        kvdb_index_section
        kvdb_bloom_section
        kvdb_meta_section
        kvdb_file_footer_section
)

kvdb_add_component(sstable_writer
    SOURCES
        src/sstable_writer.cpp
    PUBLIC_DEPS
        kvdb_status
        kvdb_arena
        kvdb_sstable
        kvdb_file
        kvdb_file_helpers
)

kvdb_add_component(sstable_loader
    SOURCES
        src/sstable_loader.cpp
    PUBLIC_DEPS
        kvdb_status
        kvdb_arena
        kvdb_sstable
        kvdb_file
        kvdb_file_helpers
)

kvdb_add_component(sstable_manager
    SOURCES
        src/sstable_manager.cpp
    PUBLIC_DEPS
        kvdb_status
        kvdb_arena
        kvdb_sstable
        kvdb_file
        kvdb_file_helpers
        kvdb_sstable_loader
        kvdb_sstable_writer
        kvdb_sstable_builder
)

kvdb_add_component(table_meta
    SOURCES
        src/table_meta.cpp
    PUBLIC_DEPS
        kvdb_arena
        kvdb_status
        kvdb_sstable
)

kvdb_add_component(level_manager
    SOURCES
        src/level_manager.cpp
    PUBLIC_DEPS
        kvdb_arena
        kvdb_status
        kvdb_table_meta
)

kvdb_add_component(merge_iterator
    SOURCES
        src/merge_iterator.cpp
    PUBLIC_DEPS
        kvdb_status
        kvdb_arena
        kvdb_table_meta
        kvdb_sstable
        kvdb_record
)

kvdb_add_component(manifest
    SOURCES
        src/manifest.cpp
    PUBLIC_DEPS
        kvdb_level_manager
        kvdb_table_meta
        kvdb_file
        kvdb_file_helpers
        kvdb_arena
)

kvdb_add_component(wal
    SOURCES
        src/wal.cpp
    PUBLIC_DEPS
        kvdb_arena
        kvdb_file
        kvdb_file_helpers
        kvdb_status
        kvdb_sstable
)

kvdb_add_component(mem_table
    SOURCES
        src/mem_table.cpp
    PUBLIC_DEPS
        kvdb_wal
        kvdb_file
        kvdb_file_helpers
        kvdb_arena
        kvdb_driver
        kvdb_db_options
        kvdb_skip_list
        kvdb_red_black_tree
        kvdb_treap
)

kvdb_add_component(engine
    SOURCES
        src/engine.cpp
    PUBLIC_DEPS
        kvdb_sstable
        kvdb_manifest
        kvdb_wal
        kvdb_mem_table
        kvdb_status
        kvdb_arena
        kvdb_db_options
        kvdb_level_manager
        kvdb_sstable_manager
        kvdb_compaction_options
        kvdb_compaction_job
        kvdb_compaction_scheduler
        kvdb_red_black_tree
        kvdb_skip_list
        kvdb_treap
)

kvdb_add_component(compaction_options
    SOURCES
        src/compaction_options.cpp
    PUBLIC_DEPS
        kvdb_status
)

kvdb_add_component(compaction_plan
    SOURCES
        src/compaction_plan.cpp
    PUBLIC_DEPS
        kvdb_status
        kvdb_arena
        kvdb_table_meta
)

kvdb_add_component(compaction_job
    SOURCES
        src/compaction_job.cpp
    PUBLIC_DEPS
        kvdb_arena
        kvdb_status
        kvdb_compaction_plan
        kvdb_table_meta
        kvdb_merge_iterator
        kvdb_sstable_iterator
        kvdb_sstable_manager
        kvdb_manifest
        kvdb_sstable_builder
)

kvdb_add_component(compaction_scheduler
    SOURCES
        src/compaction_scheduler.cpp
    PUBLIC_DEPS
        kvdb_arena
        kvdb_status
        kvdb_compaction_job
        kvdb_compaction_plan
        kvdb_table_meta
        kvdb_db_options
        kvdb_compaction_options
)

# NOTE:
# Your original CMake links kvdb_sstable_builder against kvdb_sstable_iterator,
# but this CMakeLists.txt does not define a kvdb_sstable_iterator target.
# The dependency is intentionally preserved here. Define that target elsewhere,
# or remove this dependency if the iterator is actually header-only.
kvdb_add_component(sstable_iterator
    SOURCES
        src/sstable_iterator.cpp
    PUBLIC_DEPS
        kvdb_arena
        kvdb_file
        kvdb_status
        kvdb_record
        kvdb_sstable
        kvdb_file_helpers
)

kvdb_add_component(sstable_builder
    SOURCES
        src/sstable_builder.cpp
    PUBLIC_DEPS
        kvdb_arena
        kvdb_status
        kvdb_sstable
        kvdb_file
        kvdb_file_helpers
        kvdb_mem_table
        kvdb_sstable_iterator
        kvdb_table_meta
)

kvdb_add_component(api
    SOURCES
        src/kvdb.cpp
    PUBLIC_DEPS
        kvdb_db_options
        kvdb_engine
        kvdb_status
)

get_property(
    kvdb_component_targets
    GLOBAL
    PROPERTY KVDB_COMPONENT_TARGETS
)

add_library(kvdb INTERFACE)
target_link_libraries(kvdb INTERFACE
    ${kvdb_component_targets}
)
target_include_directories(kvdb INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

add_executable(kvdb_cli
    src/main.cpp
)
target_link_libraries(kvdb_cli PRIVATE
    kvdb
)
kvdb_configure_target(kvdb_cli)

# ==============================================================================
# Tests
# ==============================================================================
#
# Convention:
#
#   package target                source file
#   ---------------------------   --------------------------------
#   kvdb_arena                    tests/arena_test.cpp
#   kvdb_wal                      tests/wal_test.cpp
#   kvdb_sstable_manager          tests/sstable_manager_test.cpp
#
# When the source exists, CMake automatically creates:
#
#   kvdb_arena_test
#   kvdb_wal_test
#   kvdb_sstable_manager_test
#
# No per-test add_executable()/gtest_discover_tests() boilerplate is needed.
# ==============================================================================

function(kvdb_discover_tests)
    file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tests/*_test.cpp"
    )

    foreach(test_source IN LISTS test_sources)
        file(RELATIVE_PATH test_relative
            "${PROJECT_SOURCE_DIR}/tests"
            "${test_source}"
        )
        string(REPLACE "\\" "_" test_name "${test_relative}")
        string(REPLACE "/" "_" test_name "${test_name}")
        string(REGEX REPLACE "\\.cpp$" "" test_name "${test_name}")
        set(test_target "kvdb_${test_name}")

        add_executable("${test_target}" "${test_source}")
        target_link_libraries("${test_target}" PRIVATE
            kvdb
            GTest::gtest_main
        )
        kvdb_configure_target("${test_target}")
        set_target_properties("${test_target}" PROPERTIES
            FOLDER "tests"
        )
        gtest_discover_tests("${test_target}"
            WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
            DISCOVERY_TIMEOUT 30
        )
    endforeach()
endfunction()

if(KVDB_ENABLE_TESTS)
    enable_testing()

    find_package(GTest CONFIG QUIET)

    if(NOT TARGET GTest::gtest_main)
        include(FetchContent)

        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )

        # Required for MSVC projects that use the shared CRT.
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

        FetchContent_MakeAvailable(googletest)
    endif()

    include(GoogleTest)

    kvdb_discover_tests()
endif()

# ==============================================================================
# Benchmarks
# ==============================================================================
#
# Optional matching convention:
#
#   kvdb_wal -> benchmarks/wal_benchmark.cpp -> kvdb_wal_benchmark
# ==============================================================================

function(kvdb_discover_component_benchmarks)
    get_property(
        component_targets
        GLOBAL
        PROPERTY KVDB_COMPONENT_TARGETS
    )

    foreach(package_target IN LISTS component_targets)
        string(REGEX REPLACE "^kvdb_" "" component_name "${package_target}")

        set(benchmark_source
            "${PROJECT_SOURCE_DIR}/benchmarks/${component_name}_benchmark.cpp"
        )
        set(benchmark_target
            "${package_target}_benchmark"
        )

        if(NOT EXISTS "${benchmark_source}")
            continue()
        endif()

        add_executable("${benchmark_target}"
            "${benchmark_source}"
        )

        target_link_libraries("${benchmark_target}" PRIVATE
            "${package_target}"
            kvdb_benchmark_timer
        )

        kvdb_configure_target("${benchmark_target}")

        set_target_properties("${benchmark_target}" PROPERTIES
            FOLDER "benchmarks"
        )

        message(STATUS
            "KVDB: discovered benchmark target ${benchmark_target} from "
            "benchmarks/${component_name}_benchmark.cpp"
        )
    endforeach()
endfunction()

if(KVDB_ENABLE_BENCHMARKS)
    add_library(kvdb_benchmark_timer
        benchmarks/timer.cpp
        benchmarks/benchmark_config.cpp
        benchmarks/process_io.cpp
    )
    target_include_directories(kvdb_benchmark_timer PUBLIC
        "${PROJECT_SOURCE_DIR}/benchmarks"
    )
    kvdb_configure_target(kvdb_benchmark_timer)
    set_target_properties(kvdb_benchmark_timer PROPERTIES
        FOLDER "benchmarks"
    )

    kvdb_discover_component_benchmarks()

    add_executable(kvdb_workload_benchmark
        benchmarks/workload_benchmark.cpp
    )
    target_link_libraries(kvdb_workload_benchmark PRIVATE
        kvdb_engine
        kvdb_benchmark_timer
    )
    kvdb_configure_target(kvdb_workload_benchmark)
    set_target_properties(kvdb_workload_benchmark PROPERTIES
        FOLDER "benchmarks"
    )
endif()
