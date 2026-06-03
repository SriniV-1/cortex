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

# ── Platform hints ─────────────────────────────────────────────────────────
# On macOS with Homebrew, add prefix so find_* commands discover packages.
# On Linux, system paths (/usr/lib, /usr/include) are searched by default.
if(APPLE)
    execute_process(
        COMMAND brew --prefix
        OUTPUT_VARIABLE HOMEBREW_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT HOMEBREW_PREFIX)
        set(HOMEBREW_PREFIX "/opt/homebrew")
    endif()
    list(PREPEND CMAKE_PREFIX_PATH "${HOMEBREW_PREFIX}")
    include_directories(SYSTEM "${HOMEBREW_PREFIX}/include")
    link_directories("${HOMEBREW_PREFIX}/lib")
    # PostgreSQL 15 (Homebrew keg-only)
    if(EXISTS "${HOMEBREW_PREFIX}/opt/postgresql@15")
        include_directories(SYSTEM "${HOMEBREW_PREFIX}/opt/postgresql@15/include")
        link_directories("${HOMEBREW_PREFIX}/opt/postgresql@15/lib")
    endif()
endif()

# ── Find packages ──────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)

pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET libpqxx)
pkg_check_modules(SPDLOG REQUIRED IMPORTED_TARGET spdlog)
pkg_check_modules(SIMDJSON REQUIRED IMPORTED_TARGET simdjson)

find_package(CURL REQUIRED)

# ── FetchContent dependencies ─────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
  rapidcheck
  GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
  GIT_TAG master
)
set(RC_ENABLE_GTEST ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rapidcheck)

find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# hiredis — try pkg-config first, fall back to find_library
pkg_check_modules(HIREDIS IMPORTED_TARGET hiredis)
if(NOT HIREDIS_FOUND)
    find_library(HIREDIS_LIB hiredis REQUIRED)
    find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h REQUIRED)
else()
    set(HIREDIS_LIB PkgConfig::HIREDIS)
    set(HIREDIS_INCLUDE_DIR ${HIREDIS_INCLUDE_DIRS})
endif()

# llhttp — try pkg-config first, fall back to find_library
pkg_check_modules(LLHTTP IMPORTED_TARGET libllhttp)
if(NOT LLHTTP_FOUND)
    find_library(LLHTTP_LIB llhttp REQUIRED)
    find_path(LLHTTP_INCLUDE_DIR llhttp.h REQUIRED)
else()
    set(LLHTTP_LIB PkgConfig::LLHTTP)
    set(LLHTTP_INCLUDE_DIR ${LLHTTP_INCLUDE_DIRS})
endif()

# ONNXRuntime — try pkg-config, then cmake config, then manual search
pkg_check_modules(ORT IMPORTED_TARGET libonnxruntime)
if(ORT_FOUND)
    set(ORT_LIB PkgConfig::ORT)
    set(ORT_INCLUDE_DIR ${ORT_INCLUDE_DIRS})
else()
    find_library(ORT_LIB onnxruntime REQUIRED)
    find_path(ORT_INCLUDE_DIR onnxruntime_cxx_api.h
        PATH_SUFFIXES onnxruntime onnxruntime/core/session
        REQUIRED)
endif()

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

# ── Analytics library (ONNX win probability model + similarity index) ────
add_library(cortex_analytics_lib STATIC
    src/analytics/WinProbModel.cpp
    src/analytics/GameStateIndex.cpp
    src/analytics/EloTracker.cpp
    src/analytics/HNSWIndex.cpp
)
target_include_directories(cortex_analytics_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${ORT_INCLUDE_DIR}
)
target_link_libraries(cortex_analytics_lib PUBLIC
    cortex_common
    PkgConfig::PQXX
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
# Platform-select the I/O poller backend.
if(APPLE)
    set(POLLER_SRC src/serving/KqueuePoller.cpp)
else()
    set(POLLER_SRC src/serving/EpollPoller.cpp)
endif()

add_library(cortex_serving_lib STATIC
    ${POLLER_SRC}
    src/serving/Connection.cpp
    src/serving/HttpServer.cpp
    src/serving/Router.cpp
    src/serving/RedisCache.cpp
    src/serving/handlers/health_handler.cpp
    src/serving/handlers/stats_handler.cpp
    src/serving/handlers/games_handler.cpp
    src/serving/handlers/search_handler.cpp
    src/serving/handlers/analytics_handler.cpp
    src/serving/handlers/metrics_handler.cpp
    src/serving/handlers/ws_handler.cpp
    src/serving/handlers/static_handler.cpp
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
    nlohmann_json::nlohmann_json
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
    tests/unit/test_router.cpp
    tests/unit/test_hnsw.cpp
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

# ── Property-based tests (RapidCheck)
add_executable(cortex_property_tests
    tests/property/test_properties.cpp
)
target_link_libraries(cortex_property_tests PRIVATE
    cortex_stream_lib
    cortex_analytics_lib
    cortex_serving_lib
    cortex_etl_lib
    GTest::gtest
    GTest::gtest_main
    rapidcheck
    rapidcheck_gtest
)
gtest_discover_tests(cortex_property_tests DISCOVERY_TIMEOUT 60)

# ── Integration tests (require DB — skipped unless CORTEX_RUN_INTEGRATION_TESTS=1)
add_executable(cortex_integration_tests
    tests/integration/test_integration.cpp
)
target_compile_definitions(cortex_integration_tests PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(cortex_integration_tests PRIVATE
    cortex_serving_lib
    cortex_analytics_lib
    cortex_etl_lib
    GTest::gtest
    GTest::gtest_main
)

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

# ── Similarity index benchmark ────────────────────────────────────────────
add_executable(cortex_similarity tests/benchmarks/bench_similarity.cpp)
target_link_libraries(cortex_similarity PRIVATE cortex_analytics_lib)

# ── Fuzz targets (requires Clang with libFuzzer) ─────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CORTEX_BUILD_FUZZ)
    add_executable(cortex_fuzz_http tests/fuzz/fuzz_http_parser.cpp)
    target_compile_options(cortex_fuzz_http PRIVATE -fsanitize=fuzzer,address)
    target_link_options(cortex_fuzz_http PRIVATE -fsanitize=fuzzer,address)
    target_link_libraries(cortex_fuzz_http PRIVATE ${LLHTTP_LIB})
    target_include_directories(cortex_fuzz_http PRIVATE ${LLHTTP_INCLUDE_DIR})

    add_executable(cortex_fuzz_ws tests/fuzz/fuzz_ws_frame.cpp)
    target_compile_options(cortex_fuzz_ws PRIVATE -fsanitize=fuzzer,address)
    target_link_options(cortex_fuzz_ws PRIVATE -fsanitize=fuzzer,address)
endif()

# ── Build info ────────────────────────────────────────────────────────────
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "libpqxx: ${PQXX_VERSION}")
message(STATUS "spdlog: ${SPDLOG_VERSION}")
message(STATUS "simdjson: ${SIMDJSON_VERSION}")
message(STATUS "OpenSSL: ${OPENSSL_VERSION}")
