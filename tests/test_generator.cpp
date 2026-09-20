#include <jsonfuzz/generator.hpp>
#include <jsonfuzz/sut.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using jsonfuzz::ByteSource;
using jsonfuzz::generate;
using jsonfuzz::Generated;
using jsonfuzz::GenOptions;
using jsonfuzz::Intent;
using jsonfuzz::ToySut;

// A deterministic pseudo-random byte stream from a seed (splitmix64).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::vector<uint8_t> bytes_from_seed(uint64_t seed, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n);
    uint64_t state = seed;
    for (size_t i = 0; i < n; ++i) {
        state += 0x9E3779B97F4A7C15ULL;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        out.push_back(static_cast<uint8_t>(z >> 32));
    }
    return out;
}

struct Counts {
    int objects = 0;
    int arrays = 0;
    int strings = 0;
    int numbers = 0;
    int bools = 0;
    int nulls = 0;
    int members = 0;
};

// A SAX counter. nlohmann's tree collapses duplicate keys (its object is a std::map), but
// the generator legitimately produces duplicate keys, so a tree walk would under-count.
// SAX sees every key/value pair as written, so it agrees with the intent record.
class SaxCounter : public nlohmann::json_sax<nlohmann::json> {
public:
    Counts counts;
    std::vector<bool> in_array;

    void value_in_array() {
        if (!in_array.empty() && in_array.back()) {
            ++counts.members;
        }
    }
    bool null() override {
        ++counts.nulls;
        value_in_array();
        return true;
    }
    bool boolean(bool) override {
        ++counts.bools;
        value_in_array();
        return true;
    }
    bool number_integer(nlohmann::json::number_integer_t) override {
        ++counts.numbers;
        value_in_array();
        return true;
    }
    bool number_unsigned(nlohmann::json::number_unsigned_t) override {
        ++counts.numbers;
        value_in_array();
        return true;
    }
    bool number_float(nlohmann::json::number_float_t, const nlohmann::json::string_t&) override {
        ++counts.numbers;
        value_in_array();
        return true;
    }
    bool string(nlohmann::json::string_t&) override {
        ++counts.strings;
        value_in_array();
        return true;
    }
    bool start_object(std::size_t) override {
        ++counts.objects;
        value_in_array();
        in_array.push_back(false);
        return true;
    }
    bool end_object() override {
        in_array.pop_back();
        return true;
    }
    bool start_array(std::size_t) override {
        ++counts.arrays;
        value_in_array();
        in_array.push_back(true);
        return true;
    }
    bool end_array() override {
        in_array.pop_back();
        return true;
    }
    bool key(nlohmann::json::string_t&) override {
        ++counts.members;
        return true;
    }
    bool binary(nlohmann::json::binary_t&) override {
        value_in_array();
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }
};

void expect_intent_matches(const Intent& intent, const std::string& text) {
    SaxCounter counter;
    ASSERT_TRUE(nlohmann::json::sax_parse(text, &counter)) << "text: " << text;
    const Counts& c = counter.counts;
    EXPECT_EQ(intent.object_count, c.objects);
    EXPECT_EQ(intent.array_count, c.arrays);
    EXPECT_EQ(intent.string_count, c.strings);
    EXPECT_EQ(intent.number_count, c.numbers);
    EXPECT_EQ(intent.bool_count, c.bools);
    EXPECT_EQ(intent.null_count, c.nulls);
    EXPECT_EQ(intent.total_members, c.members);
}

TEST(GeneratorTotalityTest, DefaultOptionsParseAndIntentAgrees) {
    const GenOptions options; // adversarial_weight = 0, so nlohmann parses everything
    std::vector<std::vector<uint8_t>> streams;
    streams.push_back({});     // empty stream
    streams.push_back({0x00}); // single zero
    streams.push_back({0xFF}); // single max
    streams.push_back(std::vector<uint8_t>(64, 0x00));
    streams.push_back(std::vector<uint8_t>(64, 0xFF));
    for (uint64_t seed = 0; seed < 200; ++seed) {
        streams.push_back(bytes_from_seed(seed, 1 + seed % 128));
    }
    int ran = 0;
    for (const auto& stream : streams) {
        ByteSource bytes(stream.data(), stream.size());
        const Generated g = generate(options, bytes);
        nlohmann::json j;
        ASSERT_NO_THROW(j = nlohmann::json::parse(g.text)) << "text: " << g.text;
        expect_intent_matches(g.intent, g.text);
        EXPECT_LE(g.intent.max_depth_reached, options.max_depth);
        ++ran;
    }
    EXPECT_GT(ran, 0);
}

TEST(GeneratorTotalityTest, EmptyStreamStillProducesADocument) {
    const GenOptions options;
    const std::vector<uint8_t> empty;
    ByteSource bytes(empty.data(), empty.size());
    const Generated g = generate(options, bytes);
    EXPECT_FALSE(g.text.empty());
    nlohmann::json j;
    EXPECT_NO_THROW(j = nlohmann::json::parse(g.text));
}

TEST(GeneratorTotalityTest, SameBytesGiveSameDocument) {
    const GenOptions options;
    const auto stream = bytes_from_seed(42, 100);
    ByteSource a(stream.data(), stream.size());
    ByteSource b(stream.data(), stream.size());
    EXPECT_EQ(generate(options, a).text, generate(options, b).text);
}

TEST(GeneratorAdversarialTest, AlphabetIsReachable) {
    // With adversarial_weight = 100 every adversarial branch is live; generate many docs
    // and confirm each class is actually produced. The ToySut (which accepts 1e309) is
    // the reference here, because nlohmann throws on the 1e309 boundary number.
    GenOptions options;
    options.max_depth = 4;
    options.max_members = 6;
    options.max_string = 8;
    options.adversarial_weight = 100;

    bool lone_surrogate = false;
    bool control_char = false;
    bool duplicate_key = false;
    bool empty_key = false;
    bool pointer_key = false;
    bool boundary_number = false;

    ToySut sut;
    const jsonfuzz::ParseConfig cfg;
    int ran = 0;
    for (uint64_t seed = 0; seed < 300; ++seed) {
        const auto stream = bytes_from_seed(seed, 200);
        ByteSource bytes(stream.data(), stream.size());
        const Generated g = generate(options, bytes);
        ASSERT_TRUE(sut.parse(g.text, cfg).accepted) << "text: " << g.text;
        lone_surrogate = lone_surrogate || g.intent.has_lone_surrogate;
        control_char = control_char || g.intent.has_control_char;
        duplicate_key = duplicate_key || g.intent.has_duplicate_key;
        empty_key = empty_key || g.intent.has_empty_key;
        pointer_key = pointer_key || g.intent.has_pointer_key;
        boundary_number = boundary_number || g.intent.has_boundary_number;
        ++ran;
    }
    EXPECT_GT(ran, 0);
    EXPECT_TRUE(lone_surrogate) << "a lone surrogate was never produced";
    EXPECT_TRUE(control_char) << "a control character was never produced";
    EXPECT_TRUE(duplicate_key) << "a duplicate key was never produced";
    EXPECT_TRUE(empty_key) << "an empty key was never produced";
    EXPECT_TRUE(pointer_key) << "a key holding / or ~ was never produced";
    EXPECT_TRUE(boundary_number) << "a grammar-boundary number was never produced";
}

TEST(GeneratorAdversarialTest, BoundaryNumberTextIsPresent) {
    GenOptions options;
    options.adversarial_weight = 100;
    bool saw_boundary = false;
    for (uint64_t seed = 0; seed < 200; ++seed) {
        const auto stream = bytes_from_seed(seed, 200);
        ByteSource bytes(stream.data(), stream.size());
        const Generated g = generate(options, bytes);
        if (g.intent.has_boundary_number) {
            saw_boundary = true;
            // The distinctive boundary numbers, plus a standalone 0 or -0 token (the
            // plain-zero boundary number is ambiguous with digits inside other numbers).
            const bool distinctive = g.text.find("1e309") != std::string::npos
                                     || g.text.find("1e-400") != std::string::npos
                                     || g.text.find("12345678901234567890") != std::string::npos;
            bool standalone_zero = false;
            for (size_t i = 0; i < g.text.size(); ++i) {
                if (g.text[i] != '0') {
                    continue;
                }
                const bool prev_ok = i == 0 || g.text[i - 1] == '[' || g.text[i - 1] == '{'
                                     || g.text[i - 1] == ',' || g.text[i - 1] == ':'
                                     || g.text[i - 1] == '-';
                const bool next_ok = i + 1 >= g.text.size() || g.text[i + 1] == ']'
                                     || g.text[i + 1] == '}' || g.text[i + 1] == ',';
                if (prev_ok && next_ok) {
                    standalone_zero = true;
                    break;
                }
            }
            EXPECT_TRUE(distinctive || standalone_zero) << "text: " << g.text;
        }
    }
    EXPECT_TRUE(saw_boundary);
}

} // namespace
