#pragma once

/// JSONFuzz fuzz readings (brief §5): the same byte input, three readings, counted
/// separately. The fuzz target is a thin wrapper over this module so the readings are
/// unit-testable; the target's JSONFUZZ_REQUIRE_REACH mode turns a starved reading into a
/// non-zero exit.
///
///   1. generate -> mutate -> run both oracles against the toy SUT;
///   2. mutate the input directly -> oracles;
///   3. generate -> the intent record must agree with the text.
///
/// The toy SUT is the deliberately-correct reference for readings 1 & 2; reading 3 uses
/// the nlohmann SAX counter as the independent truth for the generator's intent record
/// (the generator is run with non-adversarial options so the text is always parseable).

#include <jsonfuzz/generator.hpp>
#include <jsonfuzz/oracle.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jsonfuzz {

/// Which of the three readings reached its assertion on a given input.
struct ReadingReach {
    bool reading1 = false; // generate -> mutate -> oracles
    bool reading2 = false; // mutate the input directly -> oracles
    bool reading3 = false; // generate -> intent record agrees with the text
};

/// Whether each reading's primary text was ACCEPTED (so the oracle laws actually ran)
/// versus rejected (a trivial early exit). The toy SUT never throws, so for readings 1
/// & 2 an accepted parse means both O1 and O2 were evaluated to completion; for reading
/// 3, "parsed" means the intent record was compared against a real parse of the text.
/// This is the reach measurement: a reading whose texts are mostly rejected sees the
/// oracle laws only rarely.
struct ReadingAccept {
    bool reading1 = false; // reading 1's generated text was accepted
    bool reading2 = false; // reading 2's raw input was accepted
    bool reading3 = false; // reading 3's generated text parsed
};

/// The result of running the three readings on one byte input.
struct ReadingsResult {
    ReadingReach reach;
    ReadingAccept accept;
    std::vector<OracleViolation> violations; // from readings 1 & 2 (the toy SUT)
    bool intent_mismatch = false;            // reading 3: the intent disagreed with the text
};

/// Run the three readings on `data[0..size)`. Never throws. A violation or an intent
/// mismatch is a finding the caller (the fuzz target) turns into a crash.
ReadingsResult run_readings(const uint8_t* data, size_t size);

/// Reading 3's check, exposed for tests: does the generator's intent agree with the
/// text? True when the text parses AND every counted kind/member matches the intent.
bool intent_matches_text(const Intent& intent, const std::string& text);

} // namespace jsonfuzz
