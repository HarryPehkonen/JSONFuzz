#include <jsonfuzz/oracle.hpp>
#include <jsonfuzz/sut.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The two laws (brief §4), driven RED first: each law passes on the toy SUT, each law
// REJECTS a deliberately broken SUT, and the oracle never throws and never aborts on a
// SUT that throws on parse. The error path is `try { parse } catch { return; }` and the
// property runs OUTSIDE every catch — the shape JSOM's harness lacks (a broad catch
// around the round-trip property turned "{...,}" into "expected for invalid input").

namespace {

using jsonfuzz::check;
using jsonfuzz::OracleViolation;
using jsonfuzz::ParseConfig;
using jsonfuzz::ParseResult;
using jsonfuzz::SystemUnderTest;
using jsonfuzz::ToySut;

const ParseConfig kCfg;

// ---------------------------------------------------------------------------
// O1 fakes
// ---------------------------------------------------------------------------

// A strict SUT whose serialize() appends a trailing comma before a closer — the class of
// sabotage JSOM's harness could not see (its compact serializer emitted "{...,}"). O1's
// reparse of serialize() must report it.
class TrailingCommaSerializerSut : public SystemUnderTest {
public:
    ParseResult parse(std::string_view text, const ParseConfig& cfg) override {
        return toy_.parse(text, cfg);
    }
    std::string serialize() const override {
        std::string s = toy_.serialize();
        if (!s.empty() && s.back() == '}') {
            s.insert(s.size() - 1, ","); // "{...}" -> "{...,}"
        }
        return s;
    }
    std::vector<std::string> pointers(int d) const override { return toy_.pointers(d); }
    std::optional<std::string> get(std::string_view p) const override { return toy_.get(p); }

private:
    ToySut toy_;
};

// A SUT whose serialize() emits a DIFFERENT (but valid) document on every call: the
// reparse is accepted, but the serialization of the reparse is not byte-identical. The
// idempotence half of O1 must report it.
class NonIdempotentSerializerSut : public SystemUnderTest {
public:
    ParseResult parse(std::string_view, const ParseConfig&) override { return {true, ""}; }
    std::string serialize() const override {
        ++calls_;
        return "[" + std::to_string(calls_) + "]";
    }
    std::vector<std::string> pointers(int) const override { return {""}; }
    std::optional<std::string> get(std::string_view) const override { return std::nullopt; }

private:
    mutable int calls_ = 0;
};

// ---------------------------------------------------------------------------
// O2 fakes
// ---------------------------------------------------------------------------

// The S3 class: get() fails to resolve a pointer containing '/' (it never unescapes
// ~1/~0), so a pointer the SUT itself enumerated comes back empty. O2 must report it.
class WrongMemberGetterSut : public SystemUnderTest {
public:
    ParseResult parse(std::string_view, const ParseConfig&) override { return {true, ""}; }
    std::string serialize() const override { return R"({"/":1,"~":2})"; }
    std::vector<std::string> pointers(int) const override { return {"", "/~1", "/~0"}; }
    std::optional<std::string> get(std::string_view pointer) const override {
        if (pointer == "/~1") {
            return std::nullopt; // should be "1"
        }
        if (pointer == "/~0") {
            return std::nullopt; // should be "2"
        }
        return std::nullopt;
    }
};

// ---------------------------------------------------------------------------
// the throwing SUT
// ---------------------------------------------------------------------------

class ThrowingParseSut : public SystemUnderTest {
public:
    ParseResult parse(std::string_view, const ParseConfig&) override {
        throw std::runtime_error("sut threw on parse");
    }
    std::string serialize() const override { return "null"; }
    std::vector<std::string> pointers(int) const override { return {""}; }
    std::optional<std::string> get(std::string_view) const override { return std::nullopt; }
};

// ---------------------------------------------------------------------------
// O1: the round trip and idempotence
// ---------------------------------------------------------------------------

TEST(OracleTest, O1PassesOnTheToySut) {
    ToySut sut;
    EXPECT_TRUE(check(sut, R"({"a":1,"b":[true,null,"x"],"c":-0.5e3})", kCfg).empty());
}

TEST(OracleTest, O1ReportsASerializerThatEmitsInvalidJson) {
    TrailingCommaSerializerSut sut;
    const auto v = check(sut, R"({"a":1})", kCfg);
    ASSERT_FALSE(v.empty());
    bool saw_o1 = false;
    for (const auto& x : v) {
        if (x.law == OracleViolation::Law::O1) {
            saw_o1 = true;
        }
    }
    EXPECT_TRUE(saw_o1) << "expected an O1 violation for a serializer emitting {...,}";
}

TEST(OracleTest, O1ReportsANonIdempotentSerializer) {
    NonIdempotentSerializerSut sut;
    const auto v = check(sut, "1", kCfg);
    ASSERT_FALSE(v.empty());
    EXPECT_EQ(v.front().law, OracleViolation::Law::O1);
}

// ---------------------------------------------------------------------------
// O2: the pointer contract
// ---------------------------------------------------------------------------

TEST(OracleTest, O2PassesOnTheToySutIncludingEscapedKeys) {
    ToySut sut;
    EXPECT_TRUE(check(sut, R"({"a/b":1,"c~d":2,"plain":3})", kCfg).empty());
}

TEST(OracleTest, O2ReportsAPointerThatGetCannotResolve) {
    WrongMemberGetterSut sut;
    const auto v = check(sut, R"({"/":1,"~":2})", kCfg);
    ASSERT_FALSE(v.empty());
    bool saw_o2 = false;
    for (const auto& x : v) {
        if (x.law == OracleViolation::Law::O2) {
            saw_o2 = true;
        }
    }
    EXPECT_TRUE(saw_o2) << "expected an O2 violation for a get() that loses a '/' pointer";
}

// ---------------------------------------------------------------------------
// rejection and the error path
// ---------------------------------------------------------------------------

TEST(OracleTest, RejectionIsAnOutcomeNotAFinding) {
    ToySut sut;
    EXPECT_TRUE(check(sut, "not json at all", kCfg).empty());
}

TEST(OracleTest, NeverThrowsOrAbortsOnASutThatThrowsOnParse) {
    ThrowingParseSut sut;
    std::vector<OracleViolation> v;
    EXPECT_NO_THROW(v = check(sut, "whatever", kCfg));
    EXPECT_TRUE(v.empty());
}

} // namespace
