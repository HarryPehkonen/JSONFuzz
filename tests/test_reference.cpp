#include "src/jsonfuzz_reference.hpp"

#include <gtest/gtest.h>

#include <string>

// The reference's pre-pass (src/jsonfuzz_reference.hpp): a number a double cannot hold is
// tagged by its exact text so nlohmann can model the document; nothing else may change.

namespace {

using jsonfuzz::detail::tag_unmodelable_numbers;
using jsonfuzz::detail::untag_numbers;

TEST(ReferenceTagTest, TagsEveryNumberADoubleCannotHold) {
    const auto t = tag_unmodelable_numbers(R"({"a":1e309,"b":[-1e309,1e9999]})");
    EXPECT_EQ(t.tagged, 3);
    EXPECT_EQ(t.text, R"({"a":"\u0000jsonfuzz-number:1e309","b":["\u0000jsonfuzz-number:-1e309",)"
                      R"("\u0000jsonfuzz-number:1e9999"]})");
}

TEST(ReferenceTagTest, TagsAnIntegerTooLongForADouble) {
    const std::string digits = "1" + std::string(399, '0');
    const auto t = tag_unmodelable_numbers("[" + digits + "]");
    EXPECT_EQ(t.tagged, 1);
    EXPECT_EQ(t.text, "[\"\\u0000jsonfuzz-number:" + digits + "\"]");
}

TEST(ReferenceTagTest, LeavesEveryNumberADoubleCanHoldUntouched) {
    // 1e308 and the largest double are finite; 1e-400 underflows to 0, which nlohmann accepts;
    // a 24-digit integer overflows 64 bits but not a double. All still compare numerically.
    const std::string in = "[1e308,1.7976931348623157e308,1e-400,123456789012345678901234,-0,1.50]";
    const auto t = tag_unmodelable_numbers(in);
    EXPECT_EQ(t.tagged, 0);
    EXPECT_EQ(t.text, in);
}

TEST(ReferenceTagTest, NeverTagsTextInsideAString) {
    const std::string in = R"(["1e309","a\"1e309",{"1e309":true}])";
    const auto t = tag_unmodelable_numbers(in);
    EXPECT_EQ(t.tagged, 0);
    EXPECT_EQ(t.text, in);
}

TEST(ReferenceTagTest, UntagRestoresTheNumberTextInADump) {
    EXPECT_EQ(untag_numbers(R"({"a":"\u0000jsonfuzz-number:-1e309","b":"x"})"),
              R"({"a":-1e309,"b":"x"})");
    EXPECT_EQ(untag_numbers(R"("\u0000jsonfuzz-number:1e309")"), "1e309");
    EXPECT_EQ(untag_numbers("[1,2]"), "[1,2]");
}

} // namespace
