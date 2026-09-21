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
# PRIVATE, deliberately: nlohmann is the implementation's own model. No public header
# includes it (the SUT interface is strings only), so a consumer of jsonfuzz_core must not
# inherit the dependency or its include paths — e.g. the JSOM adapter links jsonfuzz_core
# without gaining a nlohmann dependency, which is the whole point of the string boundary.
target_link_libraries(jsonfuzz_core PRIVATE nlohmann_json::nlohmann_json)
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

# ================================
# Configure-time target listing
# ================================
# Print what can be built, right where `cmake -B build` is typed. Two properties make this
# worth having rather than noise:
#   * the SET is derived from the build system (BUILDSYSTEM_TARGETS), so a target added later
#     shows up by itself — the curated lines only add what the build system cannot know,
#     namely what each target is FOR. (A hand-written list would drift, which is the thing
#     the `docs` gate exists to prevent.)
#   * it re-prints on every reconfigure, which is exactly when it is needed: after a
#     CMakeLists.txt change, `cmake --build build --target NEW` fails with "No rule to make
#     target 'NEW'" until CMake has been re-run once.
# Deliberately NOT a target called `help`: the GNU Make generator already provides one
# listing targets, and Ninja has `ninja -t targets`. Deliberately at the end of the
# top-level file rather than cmake_language(DEFER CALL ...), which would raise the minimum
# CMake from 3.16 to 3.19 for a courtesy. Guarded to the top-level project so a consumer
# taking JSONFuzz in as a subdirectory (the pattern jsonTools uses for JSOM) does not get
# this block in its own configure output.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_jsonfuzz_purpose_jsonfuzz_core "the library (include/jsonfuzz + src)")
    set(_jsonfuzz_purpose_jsonfuzz "the CLI")
    set(_jsonfuzz_purpose_jsonfuzz_tests "the GoogleTest suite")
    set(_jsonfuzz_purpose_fuzz_jsonfuzz "the libFuzzer target (clang only; -DJSONFUZZ_BUILD_FUZZING=ON)")
    set(_jsonfuzz_purpose_format "reformat the sources in place with clang-format")

    set(_jsonfuzz_named "")
    foreach(_name IN ITEMS jsonfuzz_core jsonfuzz jsonfuzz_tests fuzz_jsonfuzz format)
        if(TARGET ${_name})
            list(APPEND _jsonfuzz_named ${_name})
        endif()
    endforeach()

    get_property(_jsonfuzz_built DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    set(_jsonfuzz_other "")
    foreach(_t IN LISTS _jsonfuzz_built)
        get_target_property(_jsonfuzz_alias_of ${_t} ALIASED_TARGET)
        if(_jsonfuzz_alias_of OR _t IN_LIST _jsonfuzz_named)
            continue()
        endif()
        list(APPEND _jsonfuzz_other ${_t})
    endforeach()

    message(STATUS "")
    message(STATUS "Build targets — cmake --build ${CMAKE_BINARY_DIR} --target <name>")
    foreach(_name IN LISTS _jsonfuzz_named)
        message(STATUS "  ${_name} — ${_jsonfuzz_purpose_${_name}}")
    endforeach()
    if(_jsonfuzz_other)
        message(STATUS "  (also present, not usually built by hand: ${_jsonfuzz_other})")
    endif()
    message(STATUS "")
endif()