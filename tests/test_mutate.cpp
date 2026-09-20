#include <jsonfuzz/mutate.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace {

using jsonfuzz::apply;
using jsonfuzz::mutate;
using jsonfuzz::Op;

// A rich valid document that exercises every operator's target structure: object members,
// array elements, numbers, a string with an escape, nested object, bool and null.
const char* kRichInput = R"({"a":"x\n","b":[1,2,3],"c":-0.5e3,"d":{"e":true},"f":null})";

class MutateOpTest : public ::testing::TestWithParam<Op> {};

TEST_P(MutateOpTest, ProducesDifferentBytesForSomeSeed) {
    const std::string input(kRichInput);
    bool changed = false;
    for (uint64_t seed = 0; seed < 64; ++seed) {
        const auto r = apply(input, GetParam(), seed);
        if (r.changed && r.text != input) {
            changed = true;
            break;
        }
    }
    EXPECT_TRUE(changed) << "op " << jsonfuzz::name(GetParam()) << " never changed the input";
}

TEST_P(MutateOpTest, NonJsonInputIsReturnedUnchanged) {
    const std::string input = "not json {";
    for (uint64_t seed = 0; seed < 8; ++seed) {
        const auto r = apply(input, GetParam(), seed);
        EXPECT_EQ(r.text, input);
        EXPECT_FALSE(r.changed);
    }
}

INSTANTIATE_TEST_SUITE_P(AllOps, MutateOpTest, ::testing::ValuesIn(jsonfuzz::all_ops()));

TEST(MutateTest, EveryOpHasANameAndTheyAreDistinct) {
    const auto ops = jsonfuzz::all_ops();
    std::set<std::string> names;
    for (Op op : ops) {
        const std::string n(jsonfuzz::name(op));
        EXPECT_FALSE(n.empty());
        names.insert(n);
    }
    EXPECT_EQ(names.size(), ops.size());
}

TEST(MutateTest, EntryPointIsDeterministicAndBounded) {
    const std::string input(kRichInput);
    std::vector<uint8_t> data(input.begin(), input.end());
    const size_t new_size = mutate(data.data(), data.size(), data.size(), 7);
    EXPECT_LE(new_size, data.size());

    std::vector<uint8_t> data2(input.begin(), input.end());
    const size_t new_size2 = mutate(data2.data(), data2.size(), data2.size(), 7);
    EXPECT_EQ(new_size, new_size2);
    EXPECT_EQ(std::vector<uint8_t>(data.begin(), data.begin() + new_size),
              std::vector<uint8_t>(data2.begin(), data2.begin() + new_size2));
}

TEST(MutateTest, EntryPointNonJsonIsUnchanged) {
    const std::string input = "not json";
    std::vector<uint8_t> data(input.begin(), input.end());
    const size_t new_size = mutate(data.data(), data.size(), data.size(), 3);
    EXPECT_EQ(new_size, data.size());
    EXPECT_EQ(std::string(data.begin(), data.end()), input);
}

TEST(MutateTest, EntryPointRespectsMaxSize) {
    const std::string input(kRichInput);
    std::vector<uint8_t> data(input.begin(), input.end());
    const size_t new_size = mutate(data.data(), data.size(), 4, 7);
    EXPECT_LE(new_size, 4);
}

} // namespace
