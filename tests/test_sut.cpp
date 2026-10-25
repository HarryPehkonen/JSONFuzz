#include <jsonfuzz/sut.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace {

using jsonfuzz::ParseConfig;
using jsonfuzz::SutRegistry;
using jsonfuzz::SystemUnderTest;
using jsonfuzz::ToySut;

TEST(SutRegistryTest, KnownNameResolves) {
    auto sut = jsonfuzz::default_registry().create("toy");
    ASSERT_NE(sut, nullptr);
    EXPECT_NE(dynamic_cast<ToySut*>(sut.get()), nullptr);
}

TEST(SutRegistryTest, UnknownNameIsAnErrorNotACrash) {
    auto sut = jsonfuzz::default_registry().create("no-such-sut");
    EXPECT_EQ(sut, nullptr);
}

TEST(SutRegistryTest, NamesContainsTheToy) {
    const auto names = jsonfuzz::default_registry().names();
    EXPECT_NE(std::find(names.begin(), names.end(), "toy"), names.end());
}

TEST(SutRegistryTest, ContainsReflectsRegistration) {
    EXPECT_TRUE(jsonfuzz::default_registry().contains("toy"));
    EXPECT_FALSE(jsonfuzz::default_registry().contains("missing"));
}

TEST(ToySutTest, RoundTripIsByteIdentical) {
    const std::string text = R"({"a":1,"b":[true,null,"x"],"c":-0.5e3})";
    ToySut sut;
    const ParseConfig cfg;
    ASSERT_TRUE(sut.parse(text, cfg).accepted);
    const std::string once = sut.serialize();
    ASSERT_TRUE(sut.parse(once, cfg).accepted);
    EXPECT_EQ(sut.serialize(), once);
}

TEST(ToySutTest, RejectsInvalidJson) {
    ToySut sut;
    const ParseConfig cfg;
    EXPECT_FALSE(sut.parse(R"({"a":-01})", cfg).accepted);
    EXPECT_FALSE(sut.parse(R"([1,2,])", cfg).accepted);
    EXPECT_FALSE(sut.parse(R"("unterminated)", cfg).accepted);
    EXPECT_FALSE(sut.parse(R"({"a":1.0.})", cfg).accepted);
    EXPECT_FALSE(sut.parse(R"({"a":2.e+3})", cfg).accepted);
    EXPECT_FALSE(sut.parse("{\"a\":\"raw\ncontrol\"}", cfg).accepted);
}

TEST(ToySutTest, AcceptsGrammarBoundaryNumbers) {
    ToySut sut;
    const ParseConfig cfg;
    EXPECT_TRUE(sut.parse("0", cfg).accepted);
    EXPECT_TRUE(sut.parse("-0", cfg).accepted);
    EXPECT_TRUE(sut.parse("1e309", cfg).accepted);
    EXPECT_TRUE(sut.parse("1e-400", cfg).accepted);
    EXPECT_TRUE(sut.parse("12345678901234567890", cfg).accepted);
}

TEST(ToySutTest, PointersAndGet) {
    ToySut sut;
    const ParseConfig cfg;
    ASSERT_TRUE(sut.parse(R"({"a":{"b":1},"c":[10,20]})", cfg).accepted);
    const auto ptrs = sut.pointers(8);
    EXPECT_NE(std::find(ptrs.begin(), ptrs.end(), ""), ptrs.end());
    EXPECT_NE(std::find(ptrs.begin(), ptrs.end(), "/a"), ptrs.end());
    EXPECT_NE(std::find(ptrs.begin(), ptrs.end(), "/a/b"), ptrs.end());
    EXPECT_NE(std::find(ptrs.begin(), ptrs.end(), "/c"), ptrs.end());
    EXPECT_NE(std::find(ptrs.begin(), ptrs.end(), "/c/0"), ptrs.end());
    EXPECT_NE(std::find(ptrs.begin(), ptrs.end(), "/c/1"), ptrs.end());

    EXPECT_EQ(sut.get("/a/b"), std::optional<std::string>("1"));
    EXPECT_EQ(sut.get("/c/1"), std::optional<std::string>("20"));
    EXPECT_EQ(sut.get("/nope"), std::nullopt);
    EXPECT_EQ(sut.get(""), std::optional<std::string>(R"({"a":{"b":1},"c":[10,20]})"));
}

TEST(ToySutTest, PointerEscaping) {
    ToySut sut;
    const ParseConfig cfg;
    ASSERT_TRUE(sut.parse(R"({"/":1,"~":2})", cfg).accepted);
    EXPECT_EQ(sut.get("/~1"), std::optional<std::string>("1"));
    EXPECT_EQ(sut.get("/~0"), std::optional<std::string>("2"));
}

TEST(ToySutTest, DuplicateKeysPointersAndGetAgree) {
    // Found by the fuzzer (fuzz/regressions/duplicate-empty-keys.json): with duplicate
    // keys, pointers() enumerated EVERY occurrence while get() resolved only the FIRST,
    // so a pointer into a later duplicate (here "//", a nested empty-key member) did not
    // resolve. Every pointer the SUT enumerates must resolve back to a value.
    ToySut sut;
    const ParseConfig cfg;
    ASSERT_TRUE(sut.parse(R"({"":null,"":{"":null}})", cfg).accepted);
    const auto ptrs = sut.pointers(8);
    ASSERT_FALSE(ptrs.empty());
    for (const auto& p : ptrs) {
        EXPECT_TRUE(sut.get(p).has_value()) << "pointer " << p << " did not resolve";
    }
}

TEST(ToySutTest, PointerEnumerationEscapesKeysSoThePointerRoundTrips) {
    ToySut sut;
    const ParseConfig cfg;
    ASSERT_TRUE(sut.parse(R"({"a/b":1,"c~d":2,"plain":3})", cfg).accepted);
    const auto ptrs = sut.pointers(8);

    // The escape direction: a key holding '/' must be emitted as '~1', never as a bare '/'.
    // Missing until 2026-09-20: the suite tested only the UNESCAPE direction (get("/~1")), so
    // dropping the escaping in escape_pointer_token survived all of it — measured, 55/55 green
    // with a deliberately broken escape_pointer_token. This test goes red on that sabotage.
    EXPECT_NE(std::find(ptrs.begin(), ptrs.end(), "/a~1b"), ptrs.end());
    EXPECT_NE(std::find(ptrs.begin(), ptrs.end(), "/c~0d"), ptrs.end());
    EXPECT_EQ(std::find(ptrs.begin(), ptrs.end(), "/a/b"), ptrs.end());

    // ...and every pointer the enumeration emits must resolve back to the value it names.
    EXPECT_EQ(sut.get("/a~1b"), std::optional<std::string>("1"));
    EXPECT_EQ(sut.get("/c~0d"), std::optional<std::string>("2"));
    EXPECT_EQ(sut.get("/plain"), std::optional<std::string>("3"));
}

} // namespace

// ---------------------------------------------------------------------------
// v0.3 capabilities of the toy: the write verbs, the two serialization modes and the
// number-text promise. The toy is the deliberately-correct reference every v0.3 law is
// fuzzed against, so each capability is pinned here before the law that uses it.
// ---------------------------------------------------------------------------

namespace {

using jsonfuzz::WriteResult;

TEST(ToySutWriteTest, SetAddsAFreshObjectMemberAndRemoveTakesItAway) {
    ToySut sut;
    const ParseConfig cfg;
    ASSERT_TRUE(sut.parse(R"({"a":1})", cfg).accepted);
    EXPECT_EQ(sut.set("/b", "[true]").status, WriteResult::Status::Applied);
    EXPECT_EQ(sut.serialize(), R"({"a":1,"b":[true]})");
    EXPECT_EQ(sut.get("/b/0"), std::optional<std::string>("true"));
    EXPECT_EQ(sut.remove("/b").status, WriteResult::Status::Applied);
    EXPECT_EQ(sut.serialize(), R"({"a":1})");
}

TEST(ToySutWriteTest, SetEscapesTheKeyItWrites) {
    // The pointer token is the UNESCAPED key; the canonical text must hold it JSON-escaped.
    ToySut sut;
    const ParseConfig cfg;
    ASSERT_TRUE(sut.parse("{}", cfg).accepted);
    EXPECT_EQ(sut.set("/q\"~1x~0", "1").status, WriteResult::Status::Applied);
    EXPECT_EQ(sut.serialize(), R"({"q\"/x~":1})");
    EXPECT_EQ(sut.get("/q\"~1x~0"), std::optional<std::string>("1"));
}

TEST(ToySutWriteTest, SetReplacesAnExistingMember) {
    ToySut sut;
    const ParseConfig cfg;
    ASSERT_TRUE(sut.parse(R"({"a":1})", cfg).accepted);
    EXPECT_EQ(sut.set("/a", "2").status, WriteResult::Status::Applied);
    EXPECT_EQ(sut.serialize(), R"({"a":2})");
}

TEST(ToySutWriteTest, ArraySetAppendsAtSizeOrDashAndReplacesBelowIt) {
    ToySut sut;
    const ParseConfig cfg;
    ASSERT_TRUE(sut.parse("[1]", cfg).accepted);
    EXPECT_EQ(sut.set("/1", "2").status, WriteResult::Status::Applied);
    EXPECT_EQ(sut.set("/-", "3").status, WriteResult::Status::Applied);
    EXPECT_EQ(sut.set("/0", "0").status, WriteResult::Status::Applied);
    EXPECT_EQ(sut.serialize(), "[0,2,3]");
    EXPECT_EQ(sut.set("/9", "9").status, WriteResult::Status::Rejected);
    EXPECT_EQ(sut.remove("/1").status, WriteResult::Status::Applied);
    EXPECT_EQ(sut.serialize(), "[0,3]");
}

TEST(ToySutWriteTest, RefusalsAreOutcomesNotThrows) {
    ToySut sut;
    const ParseConfig cfg;
    ASSERT_TRUE(sut.parse(R"({"a":1})", cfg).accepted);
    EXPECT_EQ(sut.set("/missing/x", "1").status, WriteResult::Status::Rejected);
    EXPECT_EQ(sut.set("/a/x", "1").status, WriteResult::Status::Rejected); // scalar parent
    EXPECT_EQ(sut.set("/b", "not json").status, WriteResult::Status::Rejected);
    EXPECT_EQ(sut.remove("/missing").status, WriteResult::Status::Rejected);
    EXPECT_EQ(sut.remove("").status, WriteResult::Status::Rejected); // the root stays
    EXPECT_EQ(sut.serialize(), R"({"a":1})");
}

TEST(ToySutModeTest, OffersCompactAndPrettyModes) {
    ToySut sut;
    const ParseConfig cfg;
    ASSERT_TRUE(sut.parse(R"({"a":[1,{}],"b":[]})", cfg).accepted);
    ASSERT_EQ(sut.serialize_mode_count(), 2);
    EXPECT_EQ(sut.serialize_mode(0), sut.serialize());
    EXPECT_EQ(sut.serialize_mode(1), "{\n  \"a\": [\n    1,\n    {}\n  ],\n  \"b\": []\n}");
}

TEST(ToySutModeTest, DeclaresNumberTextPreservation) {
    ToySut sut;
    const ParseConfig cfg;
    EXPECT_TRUE(sut.preserves_number_text());
    ASSERT_TRUE(sut.parse("[-0,1E5,1.50]", cfg).accepted);
    EXPECT_EQ(sut.serialize(), "[-0,1E5,1.50]");
}

TEST(ToySutTest, PointersUseTheDecodedKeyNotItsEscapedText) {
    // RFC 6901 tokens name the key's VALUE: the key written "a\"b" in JSON is the
    // three characters a"b, so its pointer is /a"b, not /a\"b. Found by the v0.3
    // pointer-value law (O3) the first time it ran (fuzz/regressions/escaped-key-pointer.json):
    // the toy enumerated the raw escaped
    // body, and the independent walk found no member there.
    ToySut sut;
    const ParseConfig cfg;
    ASSERT_TRUE(sut.parse(R"({"a\"b":1,"cA":2})", cfg).accepted);
    const auto ptrs = sut.pointers(8);
    EXPECT_NE(std::find(ptrs.begin(), ptrs.end(), "/a\"b"), ptrs.end());
    EXPECT_NE(std::find(ptrs.begin(), ptrs.end(), "/cA"), ptrs.end());
    EXPECT_EQ(sut.get("/a\"b"), std::optional<std::string>("1"));
    EXPECT_EQ(sut.get("/cA"), std::optional<std::string>("2"));
}

} // namespace
