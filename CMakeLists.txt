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
# Profile-guided optimization (Clang IR-PGO)
#
# Two mutually exclusive modes, both driven by cache variables:
#
#   HYDRA_PGO_GEN_DIR   — absolute path to a directory for raw
#                         profiles.  Injects `-fprofile-generate=<dir>`
#                         into compile + link flags.  Build the usual
#                         Release targets, run the training corpus,
#                         then merge with `llvm-profdata merge`.
#
#   HYDRA_PGO_USE_FILE  — absolute path to a merged .profdata file.
#                         Injects `-fprofile-use=<file>` plus
#                         -Wno-profile-instr-unprofiled / -out-of-date
#                         so stale-profile warnings don't spam during
#                         benign code churn.
#
# The orchestrator script `scripts/pgo_run.sh` sets these for you
# across two build trees (`build-pgo-gen` and `build-pgo-use`).  The
# normal Release build (`build-rel`) is untouched.
# ─────────────────────────────────────────────────────────
set(HYDRA_PGO_GEN_DIR  "" CACHE PATH "Directory for -fprofile-generate raw profiles (unset to disable).")
set(HYDRA_PGO_USE_FILE "" CACHE FILEPATH "Merged .profdata file for -fprofile-use (unset to disable).")

if(HYDRA_PGO_GEN_DIR AND HYDRA_PGO_USE_FILE)
    message(FATAL_ERROR "HYDRA_PGO_GEN_DIR and HYDRA_PGO_USE_FILE are mutually exclusive.")
endif()

if(HYDRA_PGO_GEN_DIR)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "HYDRA_PGO_GEN_DIR requires a Clang-family compiler (got ${CMAKE_CXX_COMPILER_ID}).")
    endif()
    file(MAKE_DIRECTORY "${HYDRA_PGO_GEN_DIR}")
    add_compile_options(-fprofile-generate=${HYDRA_PGO_GEN_DIR})
    add_link_options(-fprofile-generate=${HYDRA_PGO_GEN_DIR})
    message(STATUS "  HYDRA_PGO     : INSTRUMENTED (raw dir = ${HYDRA_PGO_GEN_DIR})")
endif()

if(HYDRA_PGO_USE_FILE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "HYDRA_PGO_USE_FILE requires a Clang-family compiler (got ${CMAKE_CXX_COMPILER_ID}).")
    endif()
    if(NOT EXISTS "${HYDRA_PGO_USE_FILE}")
        message(FATAL_ERROR "HYDRA_PGO_USE_FILE=${HYDRA_PGO_USE_FILE} does not exist.")
    endif()
    add_compile_options(
        -fprofile-use=${HYDRA_PGO_USE_FILE}
        -Wno-profile-instr-unprofiled
        -Wno-profile-instr-out-of-date
        # Google Benchmark compiles with -Werror; some of its own TUs
        # will naturally hash-mismatch the profile (we only train our
        # code).  Demote the PGO backend's hash-mismatch warning so
        # vendored TUs don't break the build.
        -Wno-error=backend-plugin
        -Wno-backend-plugin
    )
    add_link_options(-fprofile-use=${HYDRA_PGO_USE_FILE})
    message(STATUS "  HYDRA_PGO     : USE      (profdata = ${HYDRA_PGO_USE_FILE})")
endif()

# ─────────────────────────────────────────────────────────
# Core interface library  (header-only)
# ─────────────────────────────────────────────────────────
add_library(hydra_core INTERFACE)
target_include_directories(hydra_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

# ─────────────────────────────────────────────────────────
# Optional aarch64 assembly kernels — OFF by default
#
# HYDRA_AARCH64_ASM replaces the dual-row schoolbook leaf MAC
# (`hydra::detail::mac_row_2`) with a hand-written aarch64 kernel
# (`hydra_mac_aarch64.S`).  The kernel is ~7 % faster than the
# C++ fallback in isolated microbench at nb=12..32, but its
# out-of-line call overhead and loss of cross-call inlining
# *regress* `mul_karatsuba` by ~7-8 % and leave end-to-end pow_mod
# unchanged (or marginally worse at 2048/4096-bit).  The
# 2026-04-18 asm sprint captured this null result; the kernel
# stays in-tree as a correctness-tested A/B target for future
# experiments (full-schoolbook asm, REDC asm, etc.) but is
# OFF by default so the portable path is always what ships.
#
# To enable:  cmake -B build-rel -DHYDRA_AARCH64_ASM=ON
# ─────────────────────────────────────────────────────────
option(HYDRA_AARCH64_ASM "Enable aarch64 asm kernels (null-result A/B target — OFF by default)" OFF)

if(HYDRA_AARCH64_ASM)
    if(NOT (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|AArch64"))
        message(FATAL_ERROR "HYDRA_AARCH64_ASM=ON but target is not aarch64.")
    endif()
    enable_language(ASM)
    add_library(hydra_asm STATIC hydra_mac_aarch64.S)
    set_target_properties(hydra_asm PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(hydra_core INTERFACE HYDRA_AARCH64_ASM)
    target_link_libraries(hydra_core INTERFACE hydra_asm)
    message(STATUS "  HYDRA_AARCH64_ASM : ON (mac_row_2 → hand-written aarch64 kernel)")
else()
    message(STATUS "  HYDRA_AARCH64_ASM : OFF (default — mac_row_2 on portable C++ fallback)")
endif()

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
