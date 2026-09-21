#include <jsonfuzz/oracle.hpp>

#include <optional>
#include <utility>

namespace jsonfuzz {

// The two laws, with the measured error-path shape from the brief: `try { parse } catch
// { return; }` and the property OUTSIDE every catch. A broad catch around the property
// is exactly how JSOM's committed harness turned a serializer emitting "{...,}" into
// "expected for invalid input" (exit 0, zero findings) where the corrected shape
// reported it immediately. The oracle itself must never throw, so every SUT call is
// guarded, but the guard converts a failure into a REPORTED violation — it never converts
// a wrong answer into a pass.

namespace {

ParseResult safe_parse(SystemUnderTest& sut, std::string_view text, const ParseConfig& config) {
    try {
        return sut.parse(text, config);
    } catch (...) {
        // The SUT threw instead of answering. This is an outcome the oracle reports as a
        // failure of the contract, not something to swallow silently.
        return {false, "sut threw in parse()"};
    }
}

} // namespace

std::vector<OracleViolation> check(SystemUnderTest& sut, std::string_view text,
                                   const ParseConfig& config, int pointer_max_depth) {
    std::vector<OracleViolation> out;

    // THE ERROR PATH: the parse is the only call the brief puts inside the catch, and
    // "return" here means "nothing to check" — the model never existed, so no property
    // can be evaluated. Everything below runs OUTSIDE any catch.
    ParseResult r;
    try {
        r = sut.parse(text, config);
    } catch (...) {
        return out;
    }
    if (!r.accepted) {
        return out; // rejection is an outcome, not a finding
    }

    // O1 fixed point / round trip: serialize() reparses, and serialize() of that reparse
    // is byte-identical.
    std::string once;
    try {
        once = sut.serialize();
    } catch (...) {
        out.push_back({OracleViolation::Law::O1, "serialize() threw after an accepted parse"});
        return out;
    }
    const ParseResult r2 = safe_parse(sut, once, config);
    if (!r2.accepted) {
        out.push_back(
            {OracleViolation::Law::O1, "serialize() output does not reparse: \"" + once + "\""});
    } else {
        std::string twice;
        try {
            twice = sut.serialize();
        } catch (...) {
            twice.clear();
        }
        if (twice != once) {
            out.push_back({OracleViolation::Law::O1,
                           "serialize() of the reparse is not byte-identical to it"});
        }
    }

    // O2 pointer contract: every pointer the SUT enumerates is non-empty, is returned by
    // get() as valid JSON, and get(p) reparses. "" is the RFC 6901 whole-document
    // pointer: exempt from the non-empty clause (a document has exactly one root), but
    // its get("") is still held to the other two clauses.
    std::vector<std::string> ptrs;
    try {
        ptrs = sut.pointers(pointer_max_depth);
    } catch (...) {
        out.push_back({OracleViolation::Law::O2, "pointers() threw after an accepted parse"});
        return out;
    }

    // TWO PHASES. A SUT keeps only its last accepted parse, so reparsing a get(p) value
    // REPLACES the model the next get() would read (measured: the toy's pointers() shrank
    // to the last reparse mid-loop). Phase A resolves every pointer against the STABLE
    // model; Phase B then reparses the collected values, where clobbering no longer
    // matters. This is also what the contract means: all pointers live in the SAME parse.
    struct Resolved {
        std::string ptr;
        std::string value;
    };
    std::vector<Resolved> resolved;
    resolved.reserve(ptrs.size());
    for (const auto& p : ptrs) {
        if (p != "" && p.empty()) {
            out.push_back({OracleViolation::Law::O2,
                           "a pointer the SUT returned is empty and is not the root"});
            continue;
        }
        std::optional<std::string> got;
        try {
            got = sut.get(p);
        } catch (...) {
            out.push_back({OracleViolation::Law::O2,
                           "get(\"" + p + "\") threw for a pointer the SUT itself returned"});
            continue;
        }
        if (!got.has_value()) {
            out.push_back(
                {OracleViolation::Law::O2,
                 "get(\"" + p + "\") returned no value for a pointer the SUT itself enumerated"});
            continue;
        }
        resolved.push_back({p, std::move(*got)});
    }
    for (const auto& r : resolved) {
        const ParseResult rp = safe_parse(sut, r.value, config);
        if (!rp.accepted) {
            out.push_back({OracleViolation::Law::O2,
                           "get(\"" + r.ptr + "\") returned text that does not reparse: \""
                               + r.value + "\""});
        }
    }
    return out;
}

} // namespace jsonfuzz
