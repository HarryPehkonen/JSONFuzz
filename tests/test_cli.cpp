#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// CLI verb tests (brief §6): gen, mutate, check, --list-ops, --list-suts, and every
// documented exit code — 0 ok, 1 an oracle violation, 2 usage error, 3 no such SUT. All
// shell out to the real binary (injected by CMake), so a changed format, a swallowed
// unknown flag, or an exit code that drifted fails here.
#ifndef JSONFUZZ_CLI_BINARY
#error "JSONFUZZ_CLI_BINARY must be defined by CMake (set to the jsonfuzz executable)"
#endif

namespace {

struct RunResult {
    int status = -1; // exit code
    std::string out; // stdout + stderr (merged)
};

RunResult run(const std::vector<std::string>& args) {
    std::string cmd = "\"" + std::string(JSONFUZZ_CLI_BINARY) + "\"";
    for (const auto& a : args) {
        cmd += " \"" + a + "\"";
    }
    cmd += " 2>&1";
    std::string out;
    char buf[512];
    std::FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe == nullptr) {
        return {-1, "<popen failed>"};
    }
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
        out += buf;
    }
    const int rc = ::pclose(pipe);
    return {WEXITSTATUS(rc), out};
}

// A unique temp file per call (freed by the caller with ::remove).
std::string temp_path(const char* suffix) {
    return "/tmp/jsonfuzz_cli_" + std::to_string(getpid()) + "_" + suffix;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void write_file(const std::string& path, const std::string& content) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    ASSERT_NE(f, nullptr) << "cannot write " << path;
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
}

std::string read_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return "";
    }
    std::string out;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return out;
}

bool parses_as_json(const std::string& text) {
    try {
        const nlohmann::json parsed = nlohmann::json::parse(text);
        (void)parsed;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

TEST(CliTest, VersionPrintsExactString) { EXPECT_EQ(run({"--version"}).out, "JSONFuzz 0.1.0\n"); }

TEST(CliTest, HelpExitsZeroAndNamesTheBinary) {
    const RunResult r = run({"--help"});
    EXPECT_EQ(r.status, 0);
    EXPECT_EQ(r.out.substr(0, 14), "JSONFuzz 0.1.0");
}

TEST(CliTest, UnknownVerbFailsWithUsageError) {
    EXPECT_EQ(run({"--bogus"}).status, 2);
    EXPECT_EQ(run({"frobnicate"}).status, 2);
}

// ---- gen ------------------------------------------------------------------

TEST(CliTest, GenPrintsAValidJsonDocumentToStdout) {
    const RunResult r = run({"gen", "--seed", "7"});
    ASSERT_EQ(r.status, 0);
    EXPECT_TRUE(parses_as_json(r.out)) << "output: " << r.out;
}

TEST(CliTest, GenIsDeterministicForASeed) {
    const RunResult a = run({"gen", "--seed", "42", "--max-depth", "5"});
    const RunResult b = run({"gen", "--seed", "42", "--max-depth", "5"});
    ASSERT_EQ(a.status, 0);
    EXPECT_EQ(a.out, b.out);
}

TEST(CliTest, GenWritesIntentSidecar) {
    const std::string ip = temp_path("gen_intent.json");
    const RunResult r = run({"gen", "--seed", "3", "--intent", ip});
    ASSERT_EQ(r.status, 0);
    const std::string side = read_file(ip);
    ::remove(ip.c_str());
    ASSERT_FALSE(side.empty()) << "no intent sidecar written";
    EXPECT_NE(side.find("object_count"), std::string::npos);
    EXPECT_NE(side.find("max_depth_reached"), std::string::npos);
}

TEST(CliTest, GenUnknownFlagFails) { EXPECT_EQ(run({"gen", "--definitely-not-a-flag"}).status, 2); }

// ---- mutate ---------------------------------------------------------------

TEST(CliTest, MutateAppliesTheNamedOperatorDeterministically) {
    const std::string in = temp_path("mutate_in.json");
    const std::string in2 = temp_path("mutate_in2.json");
    write_file(in, R"({"a":1})");
    write_file(in2, R"({"a":1})");
    const RunResult a = run({"mutate", "--in", in, "--op", "wrap-in-array", "--seed", "1"});
    const RunResult b = run({"mutate", "--in", in2, "--op", "wrap-in-array", "--seed", "1"});
    ::remove(in.c_str());
    ::remove(in2.c_str());
    ASSERT_EQ(a.status, 0);
    ASSERT_EQ(b.status, 0);
    EXPECT_EQ(a.out, "[{\"a\":1}]\n");
    EXPECT_EQ(a.out, b.out);
}

TEST(CliTest, MutateUnknownOperatorFails) {
    const std::string in = temp_path("mutate_bad.json");
    write_file(in, R"({"a":1})");
    const RunResult r = run({"mutate", "--in", in, "--op", "not-an-op", "--seed", "1"});
    ::remove(in.c_str());
    EXPECT_EQ(r.status, 2);
}

// ---- check ----------------------------------------------------------------

TEST(CliTest, CheckToyOnValidJsonExitsZeroWithNoOutput) {
    const std::string in = temp_path("check_ok.json");
    write_file(in, R"({"a":1,"b":[true,null],"c~d":2})");
    const RunResult r = run({"check", "--sut", "toy", "--in", in});
    ::remove(in.c_str());
    ASSERT_EQ(r.status, 0);
    EXPECT_TRUE(r.out.empty()) << "output: " << r.out;
}

TEST(CliTest, CheckToyOnInvalidJsonExitsZero) {
    const std::string in = temp_path("check_invalid.json");
    write_file(in, "not json at all");
    const RunResult r = run({"check", "--sut", "toy", "--in", in});
    ::remove(in.c_str());
    EXPECT_EQ(r.status, 0) << "output: " << r.out;
}

TEST(CliTest, CheckUnknownSutExitsThree) {
    const std::string in = temp_path("check_no_sut.json");
    write_file(in, "{}");
    const RunResult r = run({"check", "--sut", "no-such-sut", "--in", in});
    ::remove(in.c_str());
    EXPECT_EQ(r.status, 3);
}

TEST(CliTest, CheckBrokenSutExitsOneAndPrintsTheViolation) {
    // "broken-comma" is a registered SUT that emits a trailing comma before a closer, so
    // its serializer output does not reparse. The CLI must surface it as exit 1.
    const std::string in = temp_path("check_broken.json");
    write_file(in, R"({"a":1})");
    const RunResult r = run({"check", "--sut", "broken-comma", "--in", in});
    ::remove(in.c_str());
    ASSERT_EQ(r.status, 1) << "output: " << r.out;
    EXPECT_NE(r.out.find("O1"), std::string::npos);
    EXPECT_NE(r.out.find("does not reparse"), std::string::npos);
}

TEST(CliTest, CheckMissingArgumentsIsUsageError) {
    const RunResult r = run({"check", "--sut", "toy"});
    EXPECT_EQ(r.status, 2);
}

// ---- listings -------------------------------------------------------------

TEST(CliTest, ListOpsNamesEveryOperator) {
    const RunResult r = run({"--list-ops"});
    ASSERT_EQ(r.status, 0);
    EXPECT_NE(r.out.find("truncate-at-value"), std::string::npos);
    EXPECT_NE(r.out.find("swap-members"), std::string::npos);
    EXPECT_NE(r.out.find("inject-invalid-utf8"), std::string::npos);
}

TEST(CliTest, ListSutsNamesTheToyAndTheBrokenDouble) {
    const RunResult r = run({"--list-suts"});
    ASSERT_EQ(r.status, 0);
    EXPECT_NE(r.out.find("toy"), std::string::npos);
    EXPECT_NE(r.out.find("broken-comma"), std::string::npos);
}