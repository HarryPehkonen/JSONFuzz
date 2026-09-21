// JSONFuzz libFuzzer target (brief §5) — the same byte input, three readings, counted
// separately:
//
//   1. generate -> mutate -> run both oracles against the toy SUT;
//   2. mutate the input directly -> oracles;
//   3. generate -> the intent record must agree with the text.
//
// The toy SUT is the deliberately-correct reference, so a finding here is a bug in the
// generator, the mutators, the oracles, or the toy itself — never "the parser disagreed".
//
// REACH GUARD: with JSONFUZZ_REQUIRE_REACH=1 and -runs=0 over fuzz/seeds, the process
// exits non-zero unless EVERY reading reached its assertion, and the message names which
// one starved. "At least one input got there" is the guard that hid a blind spot in a
// sibling repo (117 of 9,952 corpus inputs reached the assertion there and a class-shaped
// sabotage still survived 4.2 M executions), so each reading is counted on its own.
//
// REACH COUNTS: the same run also prints how many inputs actually reached each reading's
// oracle laws (an accepted parse) versus a trivial early exit (a rejected parse). "A
// counter is non-zero" is not evidence of reach; these numbers are.
//
// clang only: built by CMake with -DPROJECT_BUILD_FUZZING=ON.

#include <jsonfuzz/readings.hpp>
#include <jsonfuzz/version.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

// The reach guard. A static destructor runs at process exit, after libFuzzer has played
// every seed, so it can fail the whole run when a reading never reached.
struct ReachGuard {
    bool reading1 = false;
    bool reading2 = false;
    bool reading3 = false;

    // Reach COUNTS, accumulated across every input, so a run reports how many inputs
    // actually reached each reading's oracle laws (accepted parse) versus a trivial
    // early exit (rejected). "A counter is non-zero" is not evidence of reach; these
    // numbers are.
    size_t total = 0;
    size_t r1_accept = 0;
    size_t r2_accept = 0;
    size_t r3_accept = 0;

    ~ReachGuard() {
        std::fprintf(stderr, "REACH COUNTS: total=%zu r1_accept=%zu r2_accept=%zu r3_accept=%zu\n",
                     total, r1_accept, r2_accept, r3_accept);
        if (std::getenv("JSONFUZZ_REQUIRE_REACH") == nullptr) {
            return;
        }
        bool starved = false;
        if (!reading1) {
            std::fprintf(stderr, "REACH STARVED: reading 1 (generate -> mutate -> oracles)\n");
            starved = true;
        }
        if (!reading2) {
            std::fprintf(stderr, "REACH STARVED: reading 2 (mutate input -> oracles)\n");
            starved = true;
        }
        if (!reading3) {
            std::fprintf(stderr, "REACH STARVED: reading 3 (intent vs text)\n");
            starved = true;
        }
        if (starved) {
            __builtin_trap();
        }
    }
};

ReachGuard g_reach;

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const jsonfuzz::ReadingsResult res = jsonfuzz::run_readings(data, size);

    g_reach.reading1 = g_reach.reading1 || res.reach.reading1;
    g_reach.reading2 = g_reach.reading2 || res.reach.reading2;
    g_reach.reading3 = g_reach.reading3 || res.reach.reading3;

    ++g_reach.total;
    g_reach.r1_accept += res.accept.reading1 ? 1 : 0;
    g_reach.r2_accept += res.accept.reading2 ? 1 : 0;
    g_reach.r3_accept += res.accept.reading3 ? 1 : 0;

    // The counters, in the stats line, so a run shows which reading reached.
    std::fprintf(stderr, "readings r1=%d r2=%d r3=%d violations=%zu intent_mismatch=%d\n",
                 res.reach.reading1 ? 1 : 0, res.reach.reading2 ? 1 : 0, res.reach.reading3 ? 1 : 0,
                 res.violations.size(), res.intent_mismatch ? 1 : 0);

    if (!res.violations.empty() || res.intent_mismatch) {
        __builtin_trap(); // a finding: libFuzzer reports it and writes an artifact
    }
    return 0;
}
