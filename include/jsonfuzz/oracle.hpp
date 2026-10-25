#pragma once

/// JSONFuzz oracle — the v0.1 fixed-point laws (brief §4) and the v0.3 laws that catch
/// WRONG-BUT-VALID answers (the v0.4 differential oracle is deliberately not here).
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
///
/// THE v0.3 LAWS. O1/O2 only catch output that does not reparse or a pointer that does not
/// resolve; a serializer emitting perfectly legal JSON with the wrong content passes them.
/// Each law below is its own function with its own documented limit, returns a LawResult
/// (never throws — a SUT throw on the property path becomes a NAMED violation, not an
/// escaping exception), and reports whether it was evaluated or skipped, and why:
///
///   O3 pointer value. For every p in pointers(d), get(p) equals the value an INDEPENDENT
///      walk of the canonical text (nlohmann, the toolkit's own model) finds at p.
///   O4 set-then-remove. On an accepted document, set(fresh, v) then remove(fresh)
///      returns serialize() to the canonical text, and between the two get(fresh) == v.
///   O5 preset agreement. Every serialize_mode(m) reparses to the same document
///      (same serialize()), and re-serializing in mode m is byte-stable.
///   O6 number text. If the SUT declares preserves_number_text(), every number token of
///      the input appears in the canonical text of the reparsed document.
///   O7 strict accepts are lazy accepts. Whatever parses with strict_numbers = true also
///      parses with strict_numbers = false, to the same canonical text. One direction only.

#include <jsonfuzz/sut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jsonfuzz {

/// A single law violation: WHICH law, on WHAT text, at which pointer (if any), what was
/// expected and what came back — enough for a report to name the defect without a debugger.
struct OracleViolation {
    enum class Law : std::uint8_t { O1, O2, O3, O4, O5, O6, O7 };
    Law law = Law::O1;
    std::string message;
    std::string input;    // the text the law was run on
    std::string pointer;  // the pointer involved; empty when the law is not about one
    std::string expected; // empty when the message says it all
    std::string actual;
};

/// The number of laws, and a stable index for each (O1 = 0 ... O7 = 6).
constexpr std::size_t kLawCount = 7;
constexpr std::size_t law_index(OracleViolation::Law law) { return static_cast<std::size_t>(law); }

/// "O1 round-trip", "O3 pointer-value", ... — the name a report prints.
std::string_view law_name(OracleViolation::Law law);

/// One law's verdict on one input. `evaluated` is the reach measurement: false means the
/// law's property never ran (rejected input, an undeclared capability, a document the
/// reference cannot model), and `skip_reason` says which.
struct LawResult {
    std::vector<OracleViolation> violations;
    bool evaluated = false;
    std::string skip_reason;
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

/// O3 — pointer value equals the independent walk. parse(text) accepted; canonical =
/// serialize(); for every p in pointers(d): the reference (nlohmann) parses `canonical`,
/// walks p with its own RFC 6901 implementation, and get(p) must reparse (nlohmann) to an
/// EQUAL value. Catches a get() that returns the right shape with the wrong member.
/// Limits: equality is nlohmann's value equality — numbers compare numerically (1.0 == 1,
/// -0 == 0: number TEXT is O6's job) — so a wrong member holding an equal value is
/// invisible. Skipped when the reference cannot parse the canonical text (1e309, a lone
/// surrogate). A pointer that passes THROUGH a duplicated key is skipped (the reference
/// keeps the last duplicate, a SUT may keep the first; RFC 8259 leaves it undefined).
LawResult check_pointer_values(SystemUnderTest& sut, std::string_view text,
                               const ParseConfig& config, int pointer_max_depth = 256);

/// O4 — set-then-remove is the identity. parse(text) accepted; canonical = serialize().
/// For up to kSetRemoveParents container parents (from pointers(d), confirmed to be an
/// object or array by the reference walk of the canonical text), a FRESH child pointer is
/// built — an object key the parent does not hold (it contains '/' and '~', so the escaped
/// pointer path is exercised), or the array index == size — and:
///   set(fresh, v) must be Applied, get(fresh) must then equal v,
///   remove(fresh) must be Applied, and serialize() must equal canonical byte for byte.
/// Deliberately EXCLUDED: removing a member that already existed (set over an existing
/// key replaces it, so set-then-remove deletes the original — NOT the identity); a parent
/// the reference does not resolve is a skip, not a finding (it never becomes a write);
/// pointers through duplicated keys are skipped. Skipped entirely when the SUT reports
/// WriteResult::Status::Unsupported (no write path), or when the document has no
/// container to write into (a scalar root).
constexpr int kSetRemoveParents = 4;
LawResult check_set_remove(SystemUnderTest& sut, std::string_view text, const ParseConfig& config,
                           int pointer_max_depth = 256);

/// O5 — formatter-preset agreement. Skipped when serialize_mode_count() < 2. Otherwise,
/// parse(text) accepted, canonical = serialize(), and for every mode m: serialize_mode(m)
/// must reparse, the reparse's serialize() must equal canonical (the same document), and
/// serialize_mode(m) of the reparse must be byte-identical to the first (stable).
/// Limit: "the same document" is judged by the SUT's own canonical form, so a defect
/// shared by every mode AND the canonical serializer is invisible here (O3 sees some).
LawResult check_mode_agreement(SystemUnderTest& sut, std::string_view text,
                               const ParseConfig& config);

/// O6 — lazy-number text preservation, CONDITIONAL: skipped unless the SUT declares
/// preserves_number_text(). parse(text) accepted, c1 = serialize(), parse(c1) accepted,
/// c2 = serialize(); every number token of `text` must appear in c2 (as a multiset).
/// Number tokens are found by a scanner over the accepted text (outside double-quoted
/// strings, a run starting with '-', '+', '.' or a digit over [0-9.eE+-]), so it also sees
/// lazy forms nlohmann would reject. Skipped with allow_comments (a comment could hold
/// digits) and when the reference sees a duplicated key (a SUT may legitimately drop it).
LawResult check_number_text(SystemUnderTest& sut, std::string_view text, const ParseConfig& config);

/// O7 — strict accepts are a subset of lazy accepts. With `config` copied twice, differing
/// only in strict_numbers: if the strict parse is accepted, the lazy parse must be too,
/// and serialize() must agree. ONE direction only: laziness is a documented extension, so
/// lazy accepting more (or strict rejecting) is not a finding — the law is skipped.
LawResult check_strict_subset(SystemUnderTest& sut, std::string_view text,
                              const ParseConfig& config);

/// Every law, O1..O7, on one text. `evaluated[law_index(L)]` says whether law L's property
/// ran. Never throws.
struct OracleReport {
    std::vector<OracleViolation> violations;
    std::array<bool, kLawCount> evaluated{};
};
OracleReport check_all(SystemUnderTest& sut, std::string_view text, const ParseConfig& config,
                       int pointer_max_depth = 256);

} // namespace jsonfuzz
