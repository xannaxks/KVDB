

cmake_minimum_required(VERSION 3.20)
project(kvdb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

add_library(kvdb_arena
    src/arena.cpp
)

target_include_directories(kvdb_arena PUBLIC
    include
)

add_executable(arena_test
    tests/arena_test.cpp
)

target_link_libraries(arena_test PRIVATE
    kvdb_arena
    GTest::gtest_main
)

enable_testing()

include(GoogleTest)
gtest_discover_tests(arena_test)
