#include <jsonfuzz/oracle.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace jsonfuzz {

// The two laws, with the measured error-path shape from the brief: `try { parse } catch
// { return; }` and the property OUTSIDE every catch. A broad catch around the property
// is exactly how JSOM's committed harness turned a serializer emitting "{...,}" into
// "expected for invalid input" (exit 0, zero findings) where the corrected shape
// reported it immediately. The oracle itself must never throw, so every SUT call is
// guarded, but the guard converts a failure into a REPORTED violation — it never converts
// a wrong answer into a pass.

namespace {

OracleViolation violation(OracleViolation::Law law, std::string message, std::string_view input,
                          std::string pointer = {}, std::string expected = {},
                          std::string actual = {}) {
    OracleViolation v;
    v.law = law;
    v.message = std::move(message);
    v.input = std::string(input);
    v.pointer = std::move(pointer);
    v.expected = std::move(expected);
    v.actual = std::move(actual);
    return v;
}

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

namespace {

std::vector<OracleViolation> check_v01(SystemUnderTest& sut, std::string_view text,
                                       const ParseConfig& config, int pointer_max_depth,
                                       bool& accepted) {
    std::vector<OracleViolation> out;
    accepted = false;

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
    accepted = true;

    // O1 fixed point / round trip: serialize() reparses, and serialize() of that reparse
    // is byte-identical.
    std::string once;
    try {
        once = sut.serialize();
    } catch (...) {
        out.push_back(
            violation(OracleViolation::Law::O1, "serialize() threw after an accepted parse", text));
        return out;
    }
    const ParseResult r2 = safe_parse(sut, once, config);
    if (!r2.accepted) {
        out.push_back(violation(OracleViolation::Law::O1,
                                "serialize() output does not reparse: \"" + once + "\"", text, {},
                                {}, once));
    } else {
        std::string twice;
        try {
            twice = sut.serialize();
        } catch (...) {
            twice.clear();
        }
        if (twice != once) {
            out.push_back(violation(OracleViolation::Law::O1,
                                    "serialize() of the reparse is not byte-identical to it", text,
                                    {}, once, twice));
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
        out.push_back(
            violation(OracleViolation::Law::O2, "pointers() threw after an accepted parse", text));
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
            out.push_back(violation(OracleViolation::Law::O2,
                                    "a pointer the SUT returned is empty and is not the root", text,
                                    p));
            continue;
        }
        std::optional<std::string> got;
        try {
            got = sut.get(p);
        } catch (...) {
            out.push_back(violation(OracleViolation::Law::O2,
                                    "get() threw for a pointer the SUT itself returned", text, p));
            continue;
        }
        if (!got.has_value()) {
            out.push_back(violation(
                OracleViolation::Law::O2,
                "get(\"" + p + "\") returned no value for a pointer the SUT itself enumerated",
                text, p));
            continue;
        }
        resolved.push_back({p, std::move(*got)});
    }
    for (const auto& r : resolved) {
        const ParseResult rp = safe_parse(sut, r.value, config);
        if (!rp.accepted) {
            out.push_back(violation(OracleViolation::Law::O2,
                                    "get(\"" + r.ptr + "\") returned text that does not reparse: \""
                                        + r.value + "\"",
                                    text, r.ptr, {}, r.value));
        }
    }
    return out;
}

} // namespace

std::vector<OracleViolation> check(SystemUnderTest& sut, std::string_view text,
                                   const ParseConfig& config, int pointer_max_depth) {
    bool accepted = false;
    return check_v01(sut, text, config, pointer_max_depth, accepted);
}

std::string_view law_name(OracleViolation::Law law) {
    switch (law) {
    case OracleViolation::Law::O1:
        return "O1 round-trip";
    case OracleViolation::Law::O2:
        return "O2 pointer-contract";
    case OracleViolation::Law::O3:
        return "O3 pointer-value";
    case OracleViolation::Law::O4:
        return "O4 set-then-remove";
    case OracleViolation::Law::O5:
        return "O5 preset-agreement";
    case OracleViolation::Law::O6:
        return "O6 number-text";
    case OracleViolation::Law::O7:
        return "O7 strict-subset-of-lazy";
    }
    return "unknown law";
}

// ===========================================================================
// The v0.3 laws. Shared shape: the INPUT parse is the only call whose throw means "nothing
// to check" (a skip, as in check()); every later SUT call is on the property path, so its
// throw is caught and becomes a NAMED violation of the law that made the call — never an
// exception escaping to the fuzz target, never a silent pass.
// ===========================================================================

namespace {

using nlohmann::json;

// Parse the input. False (with a skip reason) when it was rejected or the SUT threw.
bool accepted_input(SystemUnderTest& sut, std::string_view text, const ParseConfig& config,
                    LawResult& r) {
    try {
        const ParseResult pr = sut.parse(text, config);
        if (!pr.accepted) {
            r.skip_reason = "the input was rejected";
            return false;
        }
    } catch (...) {
        r.skip_reason = "parse() threw on the input";
        return false;
    }
    return true;
}

// serialize(), with a throw turned into a named violation. False when it threw.
bool serialized(const SystemUnderTest& sut, OracleViolation::Law law, std::string_view text,
                LawResult& r, std::string& out) {
    try {
        out = sut.serialize();
        return true;
    } catch (...) {
        r.violations.push_back(violation(law, "serialize() threw", text));
        return false;
    }
}

std::optional<json> reference_parse(std::string_view text) {
    try {
        return json::parse(text);
    } catch (...) {
        return std::nullopt;
    }
}

std::string escape_token(const std::string& key) {
    std::string out;
    for (char c : key) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out += c;
        }
    }
    return out;
}

// The reference's view of duplicated keys. nlohmann's tree keeps only the LAST of a
// duplicated key, so its SAX interface is used to record the pointer of every member whose
// key repeats one already seen in the same object. A pointer equal to, or below, one of
// these is outside what the reference can arbitrate.
class DuplicateKeyFinder : public nlohmann::json_sax<json> {
public:
    std::vector<std::string> duplicated;

    bool null() override { return value(); }
    bool boolean(bool) override { return value(); }
    bool number_integer(json::number_integer_t) override { return value(); }
    bool number_unsigned(json::number_unsigned_t) override { return value(); }
    bool number_float(json::number_float_t, const json::string_t&) override { return value(); }
    bool string(json::string_t&) override { return value(); }
    bool binary(json::binary_t&) override { return value(); }
    bool start_object(std::size_t) override { return open(true); }
    bool start_array(std::size_t) override { return open(false); }
    bool end_object() override { return close(); }
    bool end_array() override { return close(); }
    bool key(json::string_t& k) override {
        Frame& top = stack_.back();
        top.pending = k;
        if (!top.keys.insert(k).second) {
            duplicated.push_back(top.ptr + "/" + escape_token(k));
        }
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }

private:
    struct Frame {
        bool is_object = false;
        std::string ptr;
        std::set<std::string> keys;
        std::string pending;
        std::size_t index = 0;
    };
    std::vector<Frame> stack_;

    std::string child_ptr() {
        if (stack_.empty()) {
            return "";
        }
        Frame& top = stack_.back();
        if (top.is_object) {
            return top.ptr + "/" + escape_token(top.pending);
        }
        return top.ptr + "/" + std::to_string(top.index++);
    }
    bool value() {
        (void)child_ptr();
        return true;
    }
    bool open(bool is_object) {
        Frame f;
        f.is_object = is_object;
        f.ptr = child_ptr();
        stack_.push_back(std::move(f));
        return true;
    }
    bool close() {
        stack_.pop_back();
        return true;
    }
};

std::vector<std::string> duplicated_keys(std::string_view text) {
    DuplicateKeyFinder finder;
    try {
        (void)json::sax_parse(text, &finder);
    } catch (...) {
        return finder.duplicated; // a partial scan still reports every duplicate it saw
    }
    return finder.duplicated;
}

bool through_duplicate(const std::string& p, const std::vector<std::string>& dups) {
    for (const auto& d : dups) {
        if (p == d
            || (p.size() > d.size() && p.compare(0, d.size(), d) == 0 && p[d.size()] == '/')) {
            return true;
        }
    }
    return false;
}

// The reference walk: the value at `p` in `ref`, or nullopt (with `why`) when the reference
// does not resolve it.
std::optional<json> reference_at(const json& ref, const std::string& p, std::string& why) {
    try {
        const json::json_pointer jp(p);
        if (!ref.contains(jp)) {
            why = "the reference walk finds no value there";
            return std::nullopt;
        }
        return ref.at(jp);
    } catch (const std::exception& e) {
        why = std::string("not a pointer the reference accepts: ") + e.what();
        return std::nullopt;
    }
}

// The SUT's pointers, with a throw turned into a violation. False when it threw.
bool enumerated(const SystemUnderTest& sut, int depth, OracleViolation::Law law,
                std::string_view text, LawResult& r, std::vector<std::string>& out) {
    try {
        out = sut.pointers(depth);
        return true;
    } catch (...) {
        r.violations.push_back(violation(law, "pointers() threw after an accepted parse", text));
        return false;
    }
}

// A JSON-number token scanner for O6: outside double-quoted strings, a maximal run that
// starts with '-', '+', '.' or a digit and continues over [0-9.eE+-]. Run only on text the
// SUT accepted, so it sees lazy forms (01, 1., .5) the reference would reject.
std::vector<std::string> number_tokens(std::string_view t) {
    std::vector<std::string> out;
    const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    const auto in_number = [&](char c) {
        return is_digit(c) || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-';
    };
    bool in_string = false;
    for (std::size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (in_string) {
            if (c == '\\') {
                ++i;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '-' || c == '+' || c == '.' || is_digit(c)) {
            std::size_t j = i;
            while (j < t.size() && in_number(t[j])) {
                ++j;
            }
            out.emplace_back(t.substr(i, j - i));
            i = j - 1;
        }
    }
    return out;
}

const char* const kSetValues[] = {"null", "1", "\"v\"", "[0]", "{\"k\":true}"};

} // namespace

LawResult check_pointer_values(SystemUnderTest& sut, std::string_view text,
                               const ParseConfig& config, int pointer_max_depth) {
    constexpr auto kLaw = OracleViolation::Law::O3;
    LawResult r;
    if (!accepted_input(sut, text, config, r)) {
        return r;
    }
    std::string canonical;
    if (!serialized(sut, kLaw, text, r, canonical)) {
        return r;
    }
    const std::optional<json> ref = reference_parse(canonical);
    if (!ref) {
        r.skip_reason = "the reference cannot model the canonical text";
        return r;
    }
    const std::vector<std::string> dups = duplicated_keys(canonical);
    std::vector<std::string> ptrs;
    if (!enumerated(sut, pointer_max_depth, kLaw, text, r, ptrs)) {
        return r;
    }
    r.evaluated = true;
    for (const auto& p : ptrs) {
        if (through_duplicate(p, dups)) {
            continue;
        }
        std::string why;
        const std::optional<json> expected = reference_at(*ref, p, why);
        if (!expected) {
            r.violations.push_back(
                violation(kLaw, "a pointer the SUT enumerated: " + why, text, p));
            continue;
        }
        std::optional<std::string> got;
        try {
            got = sut.get(p);
        } catch (...) {
            r.violations.push_back(violation(kLaw, "get() threw for a pointer the SUT enumerated",
                                             text, p, expected->dump()));
            continue;
        }
        if (!got) {
            r.violations.push_back(violation(kLaw, "get() returned no value", text, p,
                                             expected->dump(), "<no value>"));
            continue;
        }
        const std::optional<json> actual = reference_parse(*got);
        if (!actual || *actual != *expected) {
            r.violations.push_back(violation(kLaw,
                                             "get() disagrees with the independent walk of the "
                                             "canonical text",
                                             text, p, expected->dump(), *got));
        }
    }
    return r;
}

LawResult check_set_remove(SystemUnderTest& sut, std::string_view text, const ParseConfig& config,
                           int pointer_max_depth) {
    constexpr auto kLaw = OracleViolation::Law::O4;
    LawResult r;
    if (!accepted_input(sut, text, config, r)) {
        return r;
    }
    std::string canonical;
    if (!serialized(sut, kLaw, text, r, canonical)) {
        return r;
    }
    const std::optional<json> ref = reference_parse(canonical);
    if (!ref) {
        r.skip_reason = "the reference cannot model the canonical text";
        return r;
    }
    const std::vector<std::string> dups = duplicated_keys(canonical);
    std::vector<std::string> ptrs;
    if (!enumerated(sut, pointer_max_depth, kLaw, text, r, ptrs)) {
        return r;
    }

    // Parents: containers the REFERENCE confirms. A pointer it cannot resolve is a skip.
    struct Parent {
        std::string ptr;
        json node;
    };
    std::vector<Parent> parents;
    for (const auto& p : ptrs) {
        if (static_cast<int>(parents.size()) >= kSetRemoveParents) {
            break;
        }
        std::string why;
        if (through_duplicate(p, dups)) {
            continue;
        }
        std::optional<json> node = reference_at(*ref, p, why);
        if (node && (node->is_object() || node->is_array())) {
            parents.push_back({p, std::move(*node)});
        }
    }
    if (parents.empty()) {
        r.skip_reason = "no container to write into";
        return r;
    }

    // After a violation the model is unknown: put the canonical document back.
    const auto restore = [&]() {
        try {
            if (!sut.parse(canonical, config).accepted) {
                return false;
            }
            canonical = sut.serialize();
            return true;
        } catch (...) {
            return false;
        }
    };

    for (std::size_t i = 0; i < parents.size(); ++i) {
        const Parent& parent = parents[i];
        std::string token;
        if (parent.node.is_array()) {
            token = std::to_string(parent.node.size());
        } else {
            int n = 0;
            while (parent.node.contains("~/jf" + std::to_string(n))) {
                ++n;
            }
            token = escape_token("~/jf" + std::to_string(n));
        }
        const std::string fresh = parent.ptr + "/" + token;
        const std::string value = kSetValues[i % (sizeof(kSetValues) / sizeof(kSetValues[0]))];

        WriteResult w;
        try {
            w = sut.set(fresh, value);
        } catch (...) {
            r.violations.push_back(violation(kLaw, "set() threw", text, fresh));
            if (!restore()) {
                return r;
            }
            continue;
        }
        if (w.status == WriteResult::Status::Unsupported) {
            r.evaluated = false;
            r.skip_reason = "the SUT offers no write path (set() is Unsupported)";
            return r;
        }
        r.evaluated = true;
        if (w.status == WriteResult::Status::Rejected) {
            r.violations.push_back(violation(kLaw,
                                             "set() refused a fresh pointer under a parent that "
                                             "exists",
                                             text, fresh, "Applied", "Rejected: " + w.error));
            if (!restore()) {
                return r;
            }
            continue;
        }
        std::optional<std::string> got;
        try {
            got = sut.get(fresh);
        } catch (...) {
            got = std::nullopt;
        }
        const std::optional<json> got_json = got ? reference_parse(*got) : std::nullopt;
        if (!got_json || *got_json != json::parse(value)) {
            r.violations.push_back(violation(kLaw, "set() reported Applied but did not take", text,
                                             fresh, value, got ? *got : "<no value>"));
            if (!restore()) {
                return r;
            }
            continue;
        }
        WriteResult w2;
        try {
            w2 = sut.remove(fresh);
        } catch (...) {
            r.violations.push_back(violation(kLaw, "remove() threw", text, fresh));
            if (!restore()) {
                return r;
            }
            continue;
        }
        if (w2.status != WriteResult::Status::Applied) {
            r.violations.push_back(violation(kLaw, "remove() refused the pointer set() just wrote",
                                             text, fresh, "Applied", w2.error));
            if (!restore()) {
                return r;
            }
            continue;
        }
        std::string after;
        if (!serialized(sut, kLaw, text, r, after)) {
            return r;
        }
        if (after != canonical) {
            r.violations.push_back(violation(kLaw,
                                             "set-then-remove did not return the canonical text",
                                             text, fresh, canonical, after));
            if (!restore()) {
                return r;
            }
        }
    }
    return r;
}

LawResult check_mode_agreement(SystemUnderTest& sut, std::string_view text,
                               const ParseConfig& config) {
    constexpr auto kLaw = OracleViolation::Law::O5;
    LawResult r;
    int count = 1;
    try {
        count = sut.serialize_mode_count();
    } catch (...) {
        r.violations.push_back(violation(kLaw, "serialize_mode_count() threw", text));
        return r;
    }
    if (count < 2) {
        r.skip_reason = "the SUT offers a single serialization mode";
        return r;
    }
    if (!accepted_input(sut, text, config, r)) {
        return r;
    }
    std::string canonical;
    if (!serialized(sut, kLaw, text, r, canonical)) {
        return r;
    }
    // Every mode is taken from the SAME model before any reparse replaces it.
    std::vector<std::string> modes;
    for (int m = 0; m < count; ++m) {
        try {
            modes.push_back(sut.serialize_mode(m));
        } catch (...) {
            r.violations.push_back(
                violation(kLaw, "serialize_mode(" + std::to_string(m) + ") threw", text));
            return r;
        }
    }
    r.evaluated = true;
    for (int m = 0; m < count; ++m) {
        const std::string& once = modes[static_cast<std::size_t>(m)];
        const std::string mode = "mode " + std::to_string(m);
        if (!safe_parse(sut, once, config).accepted) {
            r.violations.push_back(
                violation(kLaw, mode + " output does not reparse", text, {}, {}, once));
            continue;
        }
        std::string same;
        if (!serialized(sut, kLaw, text, r, same)) {
            return r;
        }
        if (same != canonical) {
            r.violations.push_back(violation(kLaw, mode + " reparses to a different document", text,
                                             {}, canonical, same));
        }
        std::string twice;
        try {
            twice = sut.serialize_mode(m);
        } catch (...) {
            r.violations.push_back(violation(kLaw, mode + " threw on the reparse", text));
            continue;
        }
        if (twice != once) {
            r.violations.push_back(violation(kLaw, mode + " is not byte-stable across a reparse",
                                             text, {}, once, twice));
        }
    }
    return r;
}

LawResult check_number_text(SystemUnderTest& sut, std::string_view text,
                            const ParseConfig& config) {
    constexpr auto kLaw = OracleViolation::Law::O6;
    LawResult r;
    bool declared = false;
    try {
        declared = sut.preserves_number_text();
    } catch (...) {
        r.violations.push_back(violation(kLaw, "preserves_number_text() threw", text));
        return r;
    }
    if (!declared) {
        r.skip_reason = "the SUT does not declare number-text preservation";
        return r;
    }
    if (config.allow_comments) {
        r.skip_reason = "comments are allowed: the token scanner cannot see them";
        return r;
    }
    if (!duplicated_keys(text).empty()) {
        r.skip_reason = "the input holds a duplicated key (a SUT may drop it)";
        return r;
    }
    if (!accepted_input(sut, text, config, r)) {
        return r;
    }
    std::string c1;
    if (!serialized(sut, kLaw, text, r, c1)) {
        return r;
    }
    if (!safe_parse(sut, c1, config).accepted) {
        r.skip_reason = "the canonical text does not reparse (O1's finding, not this law's)";
        return r;
    }
    std::string c2;
    if (!serialized(sut, kLaw, text, r, c2)) {
        return r;
    }
    r.evaluated = true;
    std::map<std::string, int> available;
    for (auto& t : number_tokens(c2)) {
        ++available[t];
    }
    std::set<std::string> reported;
    for (auto& t : number_tokens(text)) {
        if (available[t] > 0) {
            --available[t];
        } else if (reported.insert(t).second) {
            r.violations.push_back(
                violation(kLaw, "a number's text did not survive the round trip", text, {}, t, c2));
        }
    }
    return r;
}

LawResult check_strict_subset(SystemUnderTest& sut, std::string_view text,
                              const ParseConfig& config) {
    constexpr auto kLaw = OracleViolation::Law::O7;
    LawResult r;
    ParseConfig strict = config;
    strict.strict_numbers = true;
    ParseConfig lazy = config;
    lazy.strict_numbers = false;
    if (!accepted_input(sut, text, strict, r)) {
        r.skip_reason = "strict: " + r.skip_reason + " (the law runs one direction only)";
        return r;
    }
    std::string strict_text;
    if (!serialized(sut, kLaw, text, r, strict_text)) {
        return r;
    }
    r.evaluated = true;
    ParseResult lr;
    try {
        lr = sut.parse(text, lazy);
    } catch (...) {
        r.violations.push_back(
            violation(kLaw, "the lazy parse threw where the strict one accepted", text));
        return r;
    }
    if (!lr.accepted) {
        r.violations.push_back(violation(kLaw, "strict accepted, lazy rejected", text, {},
                                         "accepted", "rejected: " + lr.error));
        return r;
    }
    std::string lazy_text;
    if (!serialized(sut, kLaw, text, r, lazy_text)) {
        return r;
    }
    if (lazy_text != strict_text) {
        r.violations.push_back(violation(kLaw, "strict and lazy canonical texts differ", text, {},
                                         strict_text, lazy_text));
    }
    return r;
}

OracleReport check_all(SystemUnderTest& sut, std::string_view text, const ParseConfig& config,
                       int pointer_max_depth) {
    OracleReport rep;
    bool accepted = false;
    rep.violations = check_v01(sut, text, config, pointer_max_depth, accepted);
    rep.evaluated[law_index(OracleViolation::Law::O1)] = accepted;
    rep.evaluated[law_index(OracleViolation::Law::O2)] = accepted;
    const auto add = [&rep](OracleViolation::Law law, LawResult res) {
        rep.evaluated[law_index(law)] = res.evaluated;
        rep.skip_reasons[law_index(law)] = res.skip_reason;
        for (auto& v : res.violations) {
            rep.violations.push_back(std::move(v));
        }
    };
    add(OracleViolation::Law::O3, check_pointer_values(sut, text, config, pointer_max_depth));
    add(OracleViolation::Law::O4, check_set_remove(sut, text, config, pointer_max_depth));
    add(OracleViolation::Law::O5, check_mode_agreement(sut, text, config));
    add(OracleViolation::Law::O6, check_number_text(sut, text, config));
    add(OracleViolation::Law::O7, check_strict_subset(sut, text, config));
    return rep;
}

} // namespace jsonfuzz
