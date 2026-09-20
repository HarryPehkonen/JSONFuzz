// JSONFuzz libFuzzer target — day-one (Phase A) placeholder.
//
// Phase A drives a single trivial property so the target exists and builds from the
// first commit: the artifact-identity string round-trips (version() is never empty and
// equals the CMake-generated header). The real generator/mutator/oracle readings arrive
// in Phase B; this placeholder keeps the target buildable and the seed smoke meaningful
// (every seed is executed, so libFuzzer reports "Done N runs").
//
// clang only: built by CMake with -DPROJECT_BUILD_FUZZING=ON.

#include <jsonfuzz/core.hpp>
#include <jsonfuzz/version.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    (void)data;
    (void)size;

    // The placeholder property: the version string is non-empty and matches the header.
    // A future reading replaces this with generate -> mutate -> oracle.
    const std::string v = jsonfuzz::version();
    if (v.empty() || v != std::string(JSONFUZZ_VERSION)) {
        __builtin_trap();
    }
    return 0;
}
