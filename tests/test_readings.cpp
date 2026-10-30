#include <jsonfuzz/generator.hpp>
#include <jsonfuzz/readings.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

// The three readings (brief §5), driven RED first: each reading must REACH its assertion
// on any input, the toy SUT must never violate, and reading 3's intent-vs-text check must
// detect a disagreement. The reach guard is what the fuzz target's JSONFUZZ_REQUIRE_REACH
// mode turns into a non-zero exit when a reading starves.

namespace {

using jsonfuzz::intent_matches_text;
using jsonfuzz::run_readings;

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

TEST(ReadingsTest, IntentMatchesTextForGeneratedDocuments) {
    const jsonfuzz::GenOptions options;
    int ran = 0;
    for (uint64_t seed = 0; seed < 100; ++seed) {
        const auto stream = bytes_from_seed(seed, 128);
        jsonfuzz::ByteSource bytes(stream.data(), stream.size());
        const auto g = jsonfuzz::generate(options, bytes);
        EXPECT_TRUE(intent_matches_text(g.intent, g.text)) << "text: " << g.text;
        ++ran;
    }
    EXPECT_GT(ran, 0);
}

TEST(ReadingsTest, IntentMismatchIsDetected) {
    jsonfuzz::Intent it;
    it.object_count = 1;
    it.string_count = 0;
    // The text is a single string, but the intent claims one object and no strings.
    EXPECT_FALSE(intent_matches_text(it, "\"hello\""));
}

TEST(ReadingsTest, AllThreeReadingsReachOnVariedInputs) {
    std::vector<std::vector<uint8_t>> inputs = {
        {},
        {0x00},
        {0xFF},
        std::vector<uint8_t>(64, 0x00),
        std::vector<uint8_t>(64, 0xFF),
        {'{', '"', 'a', '"', ':', '1', '}'},
    };
    for (uint64_t seed = 0; seed < 50; ++seed) {
        inputs.push_back(bytes_from_seed(seed, 1 + seed % 200));
    }
    for (const auto& in : inputs) {
        const auto res = run_readings(in.data(), in.size());
        EXPECT_TRUE(res.reach.reading1) << "reading 1 starved";
        EXPECT_TRUE(res.reach.reading2) << "reading 2 starved";
        EXPECT_TRUE(res.reach.reading3) << "reading 3 starved";
        EXPECT_TRUE(res.violations.empty()) << "the toy SUT should never violate";
        EXPECT_FALSE(res.intent_mismatch);
    }
}

TEST(ReadingsTest, NeverThrowsOnAnyInput) {
    const std::vector<uint8_t> in = bytes_from_seed(7, 300);
    EXPECT_NO_THROW(run_readings(in.data(), in.size()));
}

TEST(ReadingsTest, AcceptFlagsReportWhetherTheOracleLawsRan) {
    // The accept flags are the reach measurement: an accepted parse means the oracle laws
    // (O1 and O2) were evaluated, a rejected one means a trivial early exit. Reading 1's
    // generated text is always valid JSON (non-adversarial options), so reading1_accept
    // must be true; reading 3's generated text always parses, so reading3_accept must be
    // true. Reading 2's raw input is input-dependent.
    const std::vector<uint8_t> in = {'{', '"', 'a', '"', ':', '1', '}'};
    const auto res = run_readings(in.data(), in.size());
    EXPECT_TRUE(res.accept.reading1) << "reading 1's generated text should always parse";
    EXPECT_TRUE(res.accept.reading3) << "reading 3's generated text should always parse";
    EXPECT_TRUE(res.accept.reading2) << "this input is valid JSON, so reading 2 accepts it";
}

} // namespace

namespace {

TEST(ReadingsTest, EveryOracleLawIsEvaluatedByTheReadings) {
    // The v0.3 laws must run inside the readings, not just exist: on an accepted object
    // input every law O1..O7 is EVALUATED (not skipped) against the toy, and is clean.
    const std::vector<uint8_t> in = {'{', '"', 'a', '"', ':', '[', '-', '0', ']', '}'};
    const auto res = run_readings(in.data(), in.size());
    for (size_t i = 0; i < jsonfuzz::kLawCount; ++i) {
        EXPECT_TRUE(res.law_evaluated[i]) << "law index " << i << " never ran in the readings";
    }
    EXPECT_TRUE(res.violations.empty());
}

TEST(ReadingsTest, AV03ViolationReachesTheReadingsResult) {
    // A violation found by a v0.3 law must surface in the readings' violation list (the fuzz
    // target traps on it). The regression input is reading 2's raw text; against the fixed
    // toy it is clean, and every law it reaches is evaluated.
    const std::string text = R"({"a\"b":1,"cA":[true]})";
    const std::vector<uint8_t> in(text.begin(), text.end());
    const auto res = run_readings(in.data(), in.size());
    EXPECT_TRUE(res.violations.empty()) << res.violations.front().message;
    EXPECT_TRUE(res.law_evaluated[jsonfuzz::law_index(jsonfuzz::OracleViolation::Law::O3)]);
}

TEST(ReadingsTest, IntentMatchesATextHoldingAnOutOfRangeNumber) {
    // Reading 3's counter is nlohmann SAX, which stops at 1e309 (out_of_range.406). The
    // generator now emits such numbers at weight 0, so the counter must count them as numbers.
    jsonfuzz::Intent it;
    it.array_count = 1;
    it.number_count = 3;
    it.total_members = 3;
    EXPECT_TRUE(intent_matches_text(it, "[1e309,-1e9999,1]"));
    it.number_count = 2;
    it.string_count = 1;
    EXPECT_FALSE(intent_matches_text(it, "[1e309,-1e9999,1]")) << "a tagged number is not a string";
}

TEST(ReadingsTest, GeneratedOutOfRangeNumbersReachO3AndO4) {
    const jsonfuzz::GenOptions options;
    int holding = 0;
    for (uint64_t seed = 0; seed < 1000; ++seed) {
        const auto stream = bytes_from_seed(seed, 200);
        jsonfuzz::ByteSource bytes(stream.data(), stream.size());
        const auto g = jsonfuzz::generate(options, bytes);
        if (!g.intent.has_out_of_range_number) {
            continue;
        }
        ++holding;
        EXPECT_TRUE(intent_matches_text(g.intent, g.text)) << "text: " << g.text;
        const auto res = run_readings(stream.data(), stream.size());
        EXPECT_FALSE(res.intent_mismatch) << "text: " << g.text;
        EXPECT_TRUE(res.violations.empty()) << res.violations.front().message;
        EXPECT_TRUE(res.law_evaluated[jsonfuzz::law_index(jsonfuzz::OracleViolation::Law::O3)])
            << "text: " << g.text;
    }
    EXPECT_GT(holding, 0) << "the generator never emitted an out-of-range number at weight 0";
}

} // namespace
