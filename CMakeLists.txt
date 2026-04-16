# ─────────────────────────────────────────────────────────
# Quick-start (build directory is .gitignore-d):
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#
# The .gitignore covers:  build/  build-*/  cmake-build*/  out/
# Never run cmake from the source root without -B.
# ─────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.21)

project(
    hydra
    VERSION 0.1.0
    DESCRIPTION "Tiered multi-representation integer runtime"
    LANGUAGES CXX

)

# ─────────────────────────────────────────────────────────
# C++ standard
# ─────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─────────────────────────────────────────────────────────
# Compiler flags
# ─────────────────────────────────────────────────────────
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter  # common in templated code
)

# Release: optimise aggressively.
# Debug: address + UB sanitizers.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_BUILD_TYPE)
    add_compile_options(-O0 -g -fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    add_compile_options(-O2 -g)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -march=native)
endif()

# ─────────────────────────────────────────────────────────
# Core interface library  (header-only)
# ─────────────────────────────────────────────────────────
add_library(hydra_core INTERFACE)
target_include_directories(hydra_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

# ─────────────────────────────────────────────────────────
# Tests
# ─────────────────────────────────────────────────────────
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/hydra_test.cpp")
    add_executable(hydra_test
        hydra_test.cpp
        hydra.cpp       # implementation TU (currently a stub)
    )
    target_link_libraries(hydra_test PRIVATE hydra_core)

    # Register with CTest so `cmake --build build --target test` works.
    enable_testing()
    add_test(NAME hydra_tests COMMAND hydra_test)
endif()

# ─────────────────────────────────────────────────────────
# Benchmarks — Google Benchmark via FetchContent
# ─────────────────────────────────────────────────────────
include(FetchContent)

# Don't build Google Benchmark's own tests.
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.8.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(hydra_bench bench/bench_hydra.cpp)

target_link_libraries(hydra_bench
    PRIVATE
        hydra_core
        benchmark::benchmark
)

# Benchmarks should always be built in Release mode regardless of the
# top-level build type.  We set this per-target via compile options.
target_compile_options(hydra_bench PRIVATE
    $<$<CONFIG:Debug>:-O3 -g -DNDEBUG>
    $<$<CONFIG:RelWithDebInfo>:-O3 -g -DNDEBUG>
    $<$<CONFIG:Release>:-O3 -march=native>
)

set_target_properties(hydra_bench PROPERTIES 
    XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf-with-dsym")

# ─────────────────────────────────────────────────────────
# Optional: Boost.Multiprecision comparison benchmarks
#
#   cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DHYDRA_BENCH_BOOST=ON
#
# Boost.Multiprecision is header-only; we only need include dirs.
# The option is OFF by default so clean builds don't require Boost.
# ─────────────────────────────────────────────────────────
option(HYDRA_BENCH_BOOST "Enable Boost.Multiprecision comparison benchmarks" ON)

if(HYDRA_BENCH_BOOST)
    find_package(Boost REQUIRED)
    if(Boost_FOUND)
        message(STATUS "  Boost found   : ${Boost_INCLUDE_DIRS}")
        target_compile_definitions(hydra_bench PRIVATE HYDRA_BENCH_HAS_BOOST)
        target_include_directories(hydra_bench PRIVATE ${Boost_INCLUDE_DIRS})
    else()
        message(WARNING "HYDRA_BENCH_BOOST=ON but Boost not found; skipping.")
    endif()
endif()

# ─────────────────────────────────────────────────────────
# pow_mod comparative benchmark (standalone, no Google Benchmark dep)
# ─────────────────────────────────────────────────────────
add_executable(bench_pow_mod bench/bench_pow_mod.cpp)
target_link_libraries(bench_pow_mod PRIVATE hydra_core)
target_compile_options(bench_pow_mod PRIVATE
    $<$<CONFIG:Debug>:-O3 -g -DNDEBUG>
    $<$<CONFIG:RelWithDebInfo>:-O3 -g -DNDEBUG>
    $<$<CONFIG:Release>:-O3 -march=native>
)

# Optional backends for pow_mod benchmark
option(HYDRA_POWMOD_GMP     "Enable GMP backend in pow_mod benchmark"     OFF)
option(HYDRA_POWMOD_OPENSSL "Enable OpenSSL backend in pow_mod benchmark" OFF)

if(HYDRA_BENCH_BOOST AND Boost_FOUND)
    target_compile_definitions(bench_pow_mod PRIVATE HYDRA_POWMOD_BOOST)
    target_include_directories(bench_pow_mod PRIVATE ${Boost_INCLUDE_DIRS})
endif()

if(HYDRA_POWMOD_GMP)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GMP QUIET gmp)
    endif()
    if(NOT GMP_FOUND)
        find_library(GMP_LIBRARY gmp)
        find_path(GMP_INCLUDE_DIR gmp.h)
        if(GMP_LIBRARY AND GMP_INCLUDE_DIR)
            set(GMP_FOUND TRUE)
            set(GMP_LIBRARIES ${GMP_LIBRARY})
            set(GMP_INCLUDE_DIRS ${GMP_INCLUDE_DIR})
        endif()
    endif()
    if(GMP_FOUND)
        target_compile_definitions(bench_pow_mod PRIVATE HYDRA_POWMOD_GMP)
        target_include_directories(bench_pow_mod PRIVATE ${GMP_INCLUDE_DIRS})
        target_link_libraries(bench_pow_mod PRIVATE ${GMP_LIBRARIES})
        message(STATUS "  GMP pow_mod   : enabled")
    else()
        message(WARNING "HYDRA_POWMOD_GMP=ON but GMP not found; skipping.")
    endif()
endif()

if(HYDRA_POWMOD_OPENSSL)
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_compile_definitions(bench_pow_mod PRIVATE HYDRA_POWMOD_OPENSSL)
        target_link_libraries(bench_pow_mod PRIVATE OpenSSL::Crypto)
        message(STATUS "  OpenSSL pow_mod: enabled")
    else()
        message(WARNING "HYDRA_POWMOD_OPENSSL=ON but OpenSSL not found; skipping.")
    endif()
endif()

# ─────────────────────────────────────────────────────────
# Python interpreter (for comparison script)
# ─────────────────────────────────────────────────────────
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    message(STATUS "  Python3       : ${Python3_EXECUTABLE} (${Python3_VERSION})")
else()
    message(STATUS "  Python3       : not found — bench_compare target unavailable")
endif()

# ─────────────────────────────────────────────────────────
# Convenience targets
# ─────────────────────────────────────────────────────────

# bench — run benchmarks (raw output to terminal)
add_custom_target(bench
    COMMAND $<TARGET_FILE:hydra_bench>
    DEPENDS hydra_bench
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Running Hydra benchmarks…"
)

# bench_json — export results to JSON
set(HYDRA_BENCH_JSON "${CMAKE_BINARY_DIR}/bench_results.json")
add_custom_target(bench_json
    COMMAND $<TARGET_FILE:hydra_bench>
            --benchmark_out_format=json
            --benchmark_out=${HYDRA_BENCH_JSON}
            --benchmark_repetitions=3
            --benchmark_min_time=0.2s
    DEPENDS hydra_bench
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Running benchmarks → ${HYDRA_BENCH_JSON}"
    BYPRODUCTS "${HYDRA_BENCH_JSON}"
)

# bench_compare — JSON export + comparison report (requires Python3)
if(Python3_FOUND)
    add_custom_target(bench_compare
        COMMAND $<TARGET_FILE:hydra_bench>
                --benchmark_format=json
                --benchmark_out=${HYDRA_BENCH_JSON}
                --benchmark_repetitions=3
                --benchmark_report_aggregates_only=true
                --benchmark_min_time=0.2s
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/bench/compare.py
                ${HYDRA_BENCH_JSON}
        DEPENDS hydra_bench
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Running benchmarks and comparison report…"
        BYPRODUCTS "${HYDRA_BENCH_JSON}"
    )

    # bench_compare_md — as above but emit Markdown to bench_report.md
    set(HYDRA_BENCH_MD "${CMAKE_BINARY_DIR}/bench_report.md")
    add_custom_target(bench_compare_md
        COMMAND $<TARGET_FILE:hydra_bench>
                --benchmark_format=json
                --benchmark_out=${HYDRA_BENCH_JSON}
                --benchmark_repetitions=3
                --benchmark_report_aggregates_only=true
                --benchmark_min_time=0.2s
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/bench/compare.py
                ${HYDRA_BENCH_JSON}
                --markdown
                --output ${HYDRA_BENCH_MD}
        DEPENDS hydra_bench
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Running benchmarks → Markdown report ${HYDRA_BENCH_MD}"
        BYPRODUCTS "${HYDRA_BENCH_JSON}" "${HYDRA_BENCH_MD}"
    )
endif()

# ─────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "── Hydra build configuration ──────────────────────")
message(STATUS "  Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ standard : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Boost bench  : ${HYDRA_BENCH_BOOST}")
message(STATUS "")
message(STATUS "  Targets      : hydra_bench  bench_pow_mod  bench  bench_json  bench_compare  bench_compare_md")
message(STATUS "────────────────────────────────────────────────────")
message(STATUS "")
