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
)