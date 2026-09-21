cmake_minimum_required(VERSION 3.16)

project(JSONFuzz VERSION 0.1.0 LANGUAGES CXX)

# ================================
# Version — single source of truth
# ================================
# PROJECT_VERSION, set by project() above, is the ONLY place the version is written.
# `cmake/version.hpp.in` is configured here, so everything that needs the version —
# the CLI, tests, the `version` gate stage — reads the same value.
set(JSONFUZZ_GENERATED_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in
    ${JSONFUZZ_GENERATED_INCLUDE_DIR}/jsonfuzz/version.hpp
    @ONLY
)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(JSONFUZZ_IS_TOP_LEVEL ON)
else()
    set(JSONFUZZ_IS_TOP_LEVEL OFF)
endif()

# Compiler configuration
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The gate's build stage passes -DCMAKE_EXPORT_COMPILE_COMMANDS=ON; top-level only so a
# subdirectory consumer is not configured for it.
if(JSONFUZZ_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# ================================
# Build options
# ================================
if(JSONFUZZ_IS_TOP_LEVEL)
    option(JSONFUZZ_BUILD_TESTS "Build JSONFuzz tests" ON)
else()
    option(JSONFUZZ_BUILD_TESTS "Build JSONFuzz tests" OFF)
endif()
# libFuzzer targets need clang; the `fuzz` stage enables this in its own build dir.
option(JSONFUZZ_BUILD_FUZZING "Build the libFuzzer targets (clang only)" OFF)

include(FetchContent)

# ================================
# Dependencies
# ================================
# nlohmann/json is the mutation toolkit's own model (brief §3): the operators parse and
# restructure through it. It is NOT a SUT — the SUT interface stays parser-agnostic.
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# ================================
# Core library
# ================================
add_library(jsonfuzz_core
    src/jsonfuzz_core.cpp
    src/jsonfuzz_sut.cpp
    src/jsonfuzz_generator.cpp
src/jsonfuzz_mutate.cpp
src/jsonfuzz_oracle.cpp
    src/jsonfuzz_readings.cpp
)
target_include_directories(jsonfuzz_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${JSONFUZZ_GENERATED_INCLUDE_DIR}>
)
target_link_libraries(jsonfuzz_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(jsonfuzz_core PRIVATE cxx_std_17)

add_library(JSONFuzz::core ALIAS jsonfuzz_core)

# ================================
# CLI
# ================================
add_executable(jsonfuzz
    src/jsonfuzz_cli.cpp
)
target_link_libraries(jsonfuzz PRIVATE jsonfuzz_core)
target_include_directories(jsonfuzz PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${JSONFUZZ_GENERATED_INCLUDE_DIR}
)

# ================================
# Testing
# ================================
if(JSONFUZZ_BUILD_TESTS)
    enable_testing()

    set(INSTALL_GTEST OFF CACHE BOOL "Enable installation of googletest" FORCE)
    set(INSTALL_GMOCK OFF CACHE BOOL "Enable installation of googlemock" FORCE)

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    FetchContent_MakeAvailable(googletest)

add_executable(jsonfuzz_tests
        tests/test_version.cpp
        tests/test_cli.cpp
        tests/test_sut.cpp
        tests/test_generator.cpp
tests/test_mutate.cpp
tests/test_oracle.cpp
        tests/test_readings.cpp
    )
    target_link_libraries(jsonfuzz_tests PRIVATE jsonfuzz_core gtest gtest_main)
    target_include_directories(jsonfuzz_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # CMake's PROJECT_VERSION as a compile definition: the test asserts the generated
    # header reports exactly what project() declared.
    target_compile_definitions(jsonfuzz_tests PRIVATE JSONFUZZ_EXPECTED_VERSION="${PROJECT_VERSION}")
    # The CLI test shells out to the real binary.
    target_compile_definitions(jsonfuzz_tests PRIVATE JSONFUZZ_CLI_BINARY="$<TARGET_FILE:jsonfuzz>")

    include(GoogleTest)
    gtest_discover_tests(jsonfuzz_tests)
endif()

# ================================
# Fuzz source in the DEFAULT build (compile-only)
# ================================
# The libFuzzer target below is clang-only and lives in its own build dir, so its
# translation unit is absent from the default build's compile_commands.json and the
# tidy gate would report it as uncovered. A compile-only OBJECT library (nothing links
# it) gives fuzz/fuzz_jsonfuzz.cpp a translation unit in the default build so tidy can
# analyse it. The source itself stays g++-clean: it must not include libFuzzer-only
# headers (the same pattern JSOM uses for its fuzzer).
add_library(jsonfuzz_fuzz_obj OBJECT fuzz/fuzz_jsonfuzz.cpp)
target_include_directories(jsonfuzz_fuzz_obj PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${JSONFUZZ_GENERATED_INCLUDE_DIR}
)
target_compile_features(jsonfuzz_fuzz_obj PRIVATE cxx_std_17)

# ================================
# Fuzzing — clang only
# ================================
if(JSONFUZZ_BUILD_FUZZING)
    add_executable(fuzz_jsonfuzz
        fuzz/fuzz_jsonfuzz.cpp
    )
    target_link_libraries(fuzz_jsonfuzz PRIVATE jsonfuzz_core)
    target_include_directories(fuzz_jsonfuzz PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${JSONFUZZ_GENERATED_INCLUDE_DIR}
    )
    target_compile_options(fuzz_jsonfuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)
    target_link_options(fuzz_jsonfuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

# ================================
# Warnings as errors — OWN targets only
# ================================
# Fetched dependencies (googletest) keep their own flags: never apply -Werror to them.
foreach(jsonfuzz_target jsonfuzz_core jsonfuzz jsonfuzz_tests fuzz_jsonfuzz jsonfuzz_fuzz_obj)
    if(TARGET ${jsonfuzz_target})
        target_compile_options(${jsonfuzz_target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()

# ================================
# Code quality (developer targets)
# ================================
# Keep this glob set in step with CI_SOURCE_GLOBS in tools/ci.sh (the format gate).
file(GLOB JSONFUZZ_FORMAT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/jsonfuzz/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/*.cpp
)
find_program(CLANG_FORMAT NAMES clang-format)
if(CLANG_FORMAT)
    add_custom_target(format
        COMMAND ${CLANG_FORMAT} -i ${JSONFUZZ_FORMAT_SOURCES}
        COMMENT "Formatting source files with clang-format"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()

message(STATUS "JSONFuzz ${PROJECT_VERSION} — tests=${JSONFUZZ_BUILD_TESTS} fuzzing=${JSONFUZZ_BUILD_FUZZING}")