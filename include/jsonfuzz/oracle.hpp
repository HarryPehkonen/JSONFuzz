#pragma once

/// JSONFuzz oracle — the two fixed-point laws (brief §4).
///
/// Given a SystemUnderTest and an input text:
///
///   O1 fixed point / round trip. If parse(x) is accepted, serialize() must reparse
///      (accepted) and serialize() of that reparse must be byte-identical to it. One law
///      = round trip + idempotence at once.
///   O2 pointer contract. Every pointer string the SUT returns from pointers(d) must be
///      non-empty, must be returned by get() as valid JSON, and get(p) must itself
///      reparse.
///
/// THE ERROR PATH, HARD RULE: `try { parse } catch { return; }` and the property runs
/// OUTSIDE every catch. A broad catch around the property turns a wrong answer into
/// "expected for invalid input" — the exact way JSOM's committed harness missed a
/// serializer that emits invalid JSON ({...,}): with the round-trip property inside the
/// catch, a sabotage of the compact serializer was invisible (exit 0, zero findings);
/// moved outside the catch, the same library and seeds reported it immediately (exit 77,
/// ROUND-TRIP REPARSE FAILED). This oracle never throws and never aborts: a SUT that
/// throws on parse yields no violations, not a crash.

#include <jsonfuzz/sut.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jsonfuzz {

/// A single law violation, with the law that caught it.
struct OracleViolation {
    enum class Law : std::uint8_t { O1, O2 };
    Law law;
    std::string message;
};

/// Run both laws on `text` against `sut`. Never throws, never aborts.
///
/// - Rejection of `text` is an outcome, not a finding: no violations.
/// - A SUT that throws on parse yields no violations (the oracle cannot check a model it
///   could not build); the throw is swallowed at the parse boundary ONLY — the property
///   itself runs outside every catch.
/// - O1: parse(x) accepted => serialize() reparses AND serialize() of that reparse is
///   byte-identical to it.
/// - O2: for every pointer in pointers(max_depth): get(p) is returned, is valid JSON (it
///   reparses), and is non-empty. The RFC 6901 root pointer "" (the whole document) is
///   exempt from the non-empty clause, but its get("") is still checked for the other
///   two clauses.
std::vector<OracleViolation> check(SystemUnderTest& sut, std::string_view text,
                                   const ParseConfig& config, int pointer_max_depth = 256);

} // namespace jsonfuzz
