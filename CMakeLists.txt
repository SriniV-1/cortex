cmake_minimum_required(VERSION 3.20)
project(cortex VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Compiler flags ─────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3 -DNDEBUG)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0 -fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# ── Homebrew prefix (macOS arm64) ──────────────────────────────────────────
if(APPLE)
    # Apple Silicon Homebrew lives at /opt/homebrew
    list(PREPEND CMAKE_PREFIX_PATH "/opt/homebrew")
    include_directories(SYSTEM "/opt/homebrew/include")
    link_directories("/opt/homebrew/lib")
    # PostgreSQL 15 headers
    include_directories(SYSTEM "/opt/homebrew/opt/postgresql@15/include")
    link_directories("/opt/homebrew/opt/postgresql@15/lib")
endif()

# ── Find packages ──────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)

pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET libpqxx)
pkg_check_modules(SPDLOG REQUIRED IMPORTED_TARGET spdlog)
pkg_check_modules(SIMDJSON REQUIRED IMPORTED_TARGET simdjson)

find_package(CURL REQUIRED)
find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

# ── Common header-only interface library ──────────────────────────────────
add_library(cortex_common INTERFACE)
target_include_directories(cortex_common INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_link_libraries(cortex_common INTERFACE
    PkgConfig::SPDLOG
    Threads::Threads
)

# ── ETL library (shared between binary and tests) ─────────────────────────
add_library(cortex_etl_lib STATIC
    src/etl/NBAClient.cpp
    src/etl/BulkInserter.cpp
)
target_include_directories(cortex_etl_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(cortex_etl_lib PUBLIC
    cortex_common
    PkgConfig::PQXX
    PkgConfig::SIMDJSON
    CURL::libcurl
)

# ── ETL executable ────────────────────────────────────────────────────────
add_executable(cortex_etl src/etl/main_etl.cpp)
target_link_libraries(cortex_etl PRIVATE cortex_etl_lib)

# ── Tests ─────────────────────────────────────────────────────────────────
enable_testing()

add_executable(cortex_tests
    tests/unit/test_nba_client.cpp
    tests/unit/test_bulk_inserter.cpp
)
target_link_libraries(cortex_tests PRIVATE
    cortex_etl_lib
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(cortex_tests)

# ── Query latency benchmark ───────────────────────────────────────────────
add_executable(cortex_bench tests/benchmarks/bench_queries.cpp)
target_link_libraries(cortex_bench PRIVATE cortex_etl_lib)

# ── Build info ────────────────────────────────────────────────────────────
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "libpqxx: ${PQXX_VERSION}")
message(STATUS "spdlog: ${SPDLOG_VERSION}")
message(STATUS "simdjson: ${SIMDJSON_VERSION}")
