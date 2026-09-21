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
