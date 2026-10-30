#include <jsonfuzz/readings.hpp>

#include <jsonfuzz/mutate.hpp>
#include <jsonfuzz/sut.hpp>

#include "jsonfuzz_reference.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace jsonfuzz {

namespace {

// A SAX counter, the independent truth for reading 3. nlohmann's tree collapses duplicate
// keys (its object is a std::map), but the generator legitimately produces duplicate keys,
// so a tree walk would under-count; SAX sees every key/value pair as written and agrees
// with the intent record.
struct Counts {
    int objects = 0;
    int arrays = 0;
    int strings = 0;
    int numbers = 0;
    int bools = 0;
    int nulls = 0;
    int members = 0;
};

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

// A deterministic seed for the mutate() calls, derived from the input so the same bytes
// give the same mutations.
unsigned seed_from(const uint8_t* data, size_t size) {
    uint64_t h = 0x9E3779B97F4A7C15ULL;
    for (size_t i = 0; i < size; ++i) {
        h = (h ^ data[i]) * 0xBF58476D1CE4E5B9ULL;
    }
    return static_cast<unsigned>(h ^ (h >> 32));
}

std::string to_string(const uint8_t* data, size_t size) {
    std::string s(size, '\0');
    for (size_t i = 0; i < size; ++i) {
        s[i] = static_cast<char>(data[i]);
    }
    return s;
}

// Run every oracle law (O1..O7) against a fresh toy SUT on `text`, appending any
// violations and recording which laws were evaluated.
void check_text(const std::string& text, ReadingsResult& res) {
    ToySut sut;
    const ParseConfig cfg;
    OracleReport report = check_all(sut, text, cfg);
    for (auto& v : report.violations) {
        res.violations.push_back(std::move(v));
    }
    for (std::size_t i = 0; i < kLawCount; ++i) {
        res.law_evaluated[i] = res.law_evaluated[i] || report.evaluated[i];
    }
}

// Does the toy SUT accept `text`? The toy never throws, so acceptance means the oracle
// laws (O1 and O2) were evaluated to completion rather than hitting a trivial early exit.
bool toy_accepts(const std::string& text) {
    ToySut sut;
    const ParseConfig cfg;
    return sut.parse(text, cfg).accepted;
}

// Does nlohmann parse `text`? Used for reading 3's reach: the intent record is only
// compared against a real parse when the text is parseable.
bool text_parses(const std::string& text) {
    try {
        const nlohmann::json j = nlohmann::json::parse(detail::tag_unmodelable_numbers(text).text);
        (void)j;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

bool intent_matches_text(const Intent& intent, const std::string& text) {
    // nlohmann stops at a number a double cannot hold (out_of_range.406), and the generator
    // emits such numbers at any weight: count the TAGGED text, where each one is a string that
    // is counted back as the number it is.
    const detail::TaggedText tagged = detail::tag_unmodelable_numbers(text);
    SaxCounter counter;
    if (!nlohmann::json::sax_parse(tagged.text, &counter)) {
        return false;
    }
    Counts c = counter.counts;
    c.strings -= tagged.tagged;
    c.numbers += tagged.tagged;
    return intent.object_count == c.objects && intent.array_count == c.arrays
           && intent.string_count == c.strings && intent.number_count == c.numbers
           && intent.bool_count == c.bools && intent.null_count == c.nulls
           && intent.total_members == c.members;
}

ReadingsResult run_readings(const uint8_t* data, size_t size) {
    ReadingsResult out;

    // The generator is run with non-adversarial options so reading 3's text is always
    // parseable by the independent nlohmann counter.
    const GenOptions options;
    ByteSource bytes(data, size);
    const Generated g = generate(options, bytes);

    // Reading 1: generate -> mutate -> oracles (every law, O1..O7).
    {
        check_text(g.text, out);
        std::vector<uint8_t> buf(g.text.begin(), g.text.end());
        const size_t ns = mutate(buf.data(), buf.size(), buf.size(), seed_from(data, size));
        check_text(to_string(buf.data(), ns), out);
        out.reach.reading1 = true;
        out.accept.reading1 = toy_accepts(g.text);
    }

    // Reading 2: mutate the input directly -> oracles.
    {
        check_text(to_string(data, size), out);
        std::vector<uint8_t> buf(data, data + size);
        const size_t ns = mutate(buf.data(), buf.size(), buf.size(), seed_from(data, size));
        check_text(to_string(buf.data(), ns), out);
        out.reach.reading2 = true;
        out.accept.reading2 = toy_accepts(to_string(data, size));
    }

    // Reading 3: generate -> the intent record must agree with the text.
    out.intent_mismatch = !intent_matches_text(g.intent, g.text);
    out.reach.reading3 = true;
    out.accept.reading3 = text_parses(g.text);

    return out;
}

} // namespace jsonfuzz
