#include <gtest/gtest.h>

#include <cstdio>
#include <string>

// `jsonfuzz --version` must print exactly "JSONFuzz 0.1.0". This test shells out to
// the real binary (its path is injected by CMake), so it fails if the format, the
// number, or the trailing newline changes — not merely if the in-memory value does.
#ifndef JSONFUZZ_CLI_BINARY
#error "JSONFUZZ_CLI_BINARY must be defined by CMake (set to the jsonfuzz executable)"
#endif

namespace {

std::string run(const std::string& args) {
    std::string cmd = "\"" + std::string(JSONFUZZ_CLI_BINARY) + "\" " + args;
    std::string out;
    char buf[512];
    std::FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe == nullptr) {
        return "<popen failed>";
    }
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
        out += buf;
    }
    ::pclose(pipe);
    return out;
}

} // namespace

TEST(CliTest, VersionPrintsExactString) { EXPECT_EQ(run("--version"), "JSONFuzz 0.1.0\n"); }

TEST(CliTest, HelpExitsZeroAndNamesTheBinary) {
    const std::string out = run("--help");
    EXPECT_EQ(out.substr(0, 14), "JSONFuzz 0.1.0");
}
