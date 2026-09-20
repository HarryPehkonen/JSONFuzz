#include <jsonfuzz/core.hpp>
#include <jsonfuzz/version.hpp>

#include <gtest/gtest.h>

#include <string>

// The one version number: what the generated header reports must equal what CMake
// defined for project(JSONFuzz VERSION ...). CMake injects PROJECT_VERSION here as
// JSONFUZZ_EXPECTED_VERSION, so if either the project() line or the header drifts
// from the other, this test fails.
#ifndef JSONFUZZ_EXPECTED_VERSION
#error "JSONFUZZ_EXPECTED_VERSION must be defined by CMake (set to PROJECT_VERSION)"
#endif

TEST(VersionTest, HeaderMatchesProjectVersion) {
    EXPECT_STREQ(JSONFUZZ_VERSION, JSONFUZZ_EXPECTED_VERSION);
}

TEST(VersionTest, CoreReportsTheSameVersion) {
    EXPECT_EQ(jsonfuzz::version(), std::string(JSONFUZZ_VERSION));
}

TEST(VersionTest, MacroComponentsAgreeWithFullVersion) {
    const std::string full = JSONFUZZ_VERSION;
    EXPECT_EQ(JSONFUZZ_VERSION_MAJOR, 0);
    EXPECT_EQ(JSONFUZZ_VERSION_MINOR, 1);
    EXPECT_EQ(JSONFUZZ_VERSION_PATCH, 0);
    EXPECT_EQ(full, "0.1.0");
}
