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
find_package(OpenSSL REQUIRED)

# hiredis
find_library(HIREDIS_LIB hiredis HINTS /opt/homebrew/lib REQUIRED)
set(HIREDIS_INCLUDE_DIR /opt/homebrew/include)

# llhttp
find_library(LLHTTP_LIB llhttp HINTS /opt/homebrew/lib REQUIRED)
set(LLHTTP_INCLUDE_DIR /opt/homebrew/include)

# ONNXRuntime (Homebrew arm64)
find_library(ORT_LIB onnxruntime
    HINTS /opt/homebrew/lib
    REQUIRED)
set(ORT_INCLUDE_DIR /opt/homebrew/include/onnxruntime)

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
    src/etl/LiveIngestor.cpp
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

# ── Analytics library (ONNX win probability model) ───────────────────────
add_library(cortex_analytics_lib STATIC
    src/analytics/WinProbModel.cpp
)
target_include_directories(cortex_analytics_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${ORT_INCLUDE_DIR}
)
target_link_libraries(cortex_analytics_lib PUBLIC
    cortex_common
    ${ORT_LIB}
)

# ── Stream processing library ─────────────────────────────────────────────
add_library(cortex_stream_lib STATIC
    src/stream/StreamProcessor.cpp
    src/stream/StatAccumulator.cpp
)
target_include_directories(cortex_stream_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(cortex_stream_lib PUBLIC
    cortex_common
    cortex_analytics_lib
)

# ── Serving library (HTTP/WebSocket/Redis) ────────────────────────────────
add_library(cortex_serving_lib STATIC
    src/serving/KqueuePoller.cpp
    src/serving/HttpServer.cpp
    src/serving/RedisCache.cpp
)
target_include_directories(cortex_serving_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${HIREDIS_INCLUDE_DIR}
    ${LLHTTP_INCLUDE_DIR}
)
target_link_libraries(cortex_serving_lib PUBLIC
    cortex_common
    cortex_stream_lib
    cortex_etl_lib
    ${HIREDIS_LIB}
    ${LLHTTP_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
)

# ── ETL executable ────────────────────────────────────────────────────────
add_executable(cortex_etl src/etl/main_etl.cpp)
target_link_libraries(cortex_etl PRIVATE cortex_etl_lib)

# ── Tests ─────────────────────────────────────────────────────────────────
enable_testing()

add_executable(cortex_tests
    tests/unit/test_nba_client.cpp
    tests/unit/test_bulk_inserter.cpp
    tests/unit/test_stream.cpp
    tests/unit/test_serving.cpp
)
target_compile_definitions(cortex_tests PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(cortex_tests PRIVATE
    cortex_etl_lib
    cortex_stream_lib
    cortex_analytics_lib
    cortex_serving_lib
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(cortex_tests DISCOVERY_TIMEOUT 30)

# ── Query latency benchmark ───────────────────────────────────────────────
add_executable(cortex_bench tests/benchmarks/bench_queries.cpp)
target_link_libraries(cortex_bench PRIVATE cortex_etl_lib)

# ── Throughput benchmark ──────────────────────────────────────────────────
add_executable(cortex_throughput tests/benchmarks/bench_throughput.cpp)
target_link_libraries(cortex_throughput PRIVATE cortex_stream_lib)

# ── Analytics server ──────────────────────────────────────────────────────
add_executable(cortex_server src/serving/main_server.cpp)
target_link_libraries(cortex_server PRIVATE cortex_serving_lib)
target_include_directories(cortex_server PRIVATE ${ORT_INCLUDE_DIR})

# ── WebSocket load test ───────────────────────────────────────────────────
add_executable(cortex_ws_load tests/benchmarks/bench_ws_load.cpp)
target_link_libraries(cortex_ws_load PRIVATE cortex_serving_lib)

# ── Build info ────────────────────────────────────────────────────────────
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "libpqxx: ${PQXX_VERSION}")
message(STATUS "spdlog: ${SPDLOG_VERSION}")
message(STATUS "simdjson: ${SIMDJSON_VERSION}")
message(STATUS "OpenSSL: ${OPENSSL_VERSION}")
