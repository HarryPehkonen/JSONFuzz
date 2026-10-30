#include <jsonfuzz/oracle.hpp>
#include <jsonfuzz/sut.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The v0.3 laws (O3..O7), driven RED first. Each law has the same four tests: it passes
// (and is EVALUATED, not skipped) on the deliberately-correct toy; it reports a SUT that is
// wrong in exactly the way the law exists to catch — with valid JSON, so O1/O2 cannot; it
// is SKIPPED (with a reason) where it does not apply; and a SUT that throws on the property
// path yields a NAMED violation instead of an escaping exception.

namespace {

using jsonfuzz::check_all;
using jsonfuzz::check_mode_agreement;
using jsonfuzz::check_number_text;
using jsonfuzz::check_pointer_values;
using jsonfuzz::check_set_remove;
using jsonfuzz::check_strict_subset;
using jsonfuzz::kLawCount;
using jsonfuzz::law_index;
using jsonfuzz::LawResult;
using jsonfuzz::OracleViolation;
using jsonfuzz::ParseConfig;
using jsonfuzz::ParseResult;
using jsonfuzz::SystemUnderTest;
using jsonfuzz::ToySut;
using jsonfuzz::WriteResult;
using Law = OracleViolation::Law;

const ParseConfig kCfg;

// A SUT that forwards everything to a correct toy; each fake overrides one verb to break it.
class ToyBacked : public SystemUnderTest {
public:
    ParseResult parse(std::string_view text, const ParseConfig& cfg) override {
        return toy_.parse(text, cfg);
    }
    std::string serialize() const override { return toy_.serialize(); }
    std::vector<std::string> pointers(int d) const override { return toy_.pointers(d); }
    std::optional<std::string> get(std::string_view p) const override { return toy_.get(p); }
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    WriteResult set(std::string_view p, std::string_view v) override { return toy_.set(p, v); }
    WriteResult remove(std::string_view p) override { return toy_.remove(p); }
    int serialize_mode_count() const override { return toy_.serialize_mode_count(); }
    std::string serialize_mode(int m) const override { return toy_.serialize_mode(m); }
    bool preserves_number_text() const override { return toy_.preserves_number_text(); }

protected:
    ToySut toy_;
};

bool has_law(const std::vector<OracleViolation>& v, Law law) {
    for (const auto& x : v) {
        if (x.law == law) {
            return true;
        }
    }
    return false;
}

const OracleViolation* first_of(const std::vector<OracleViolation>& v, Law law) {
    for (const auto& x : v) {
        if (x.law == law) {
            return &x;
        }
    }
    return nullptr;
}

// ===========================================================================
// O3 — pointer value equals the independent walk
// ===========================================================================

// The B3 S2 class, made VALID: get() of an object member returns the NEXT sibling's value.
// The text reparses, so O2 is blind to it; only an independent walk can see it.
class SiblingGetterSut : public ToyBacked {
public:
    std::optional<std::string> get(std::string_view p) const override {
        if (p == "/a") {
            return toy_.get("/b");
        }
        return toy_.get(p);
    }
};

class ThrowingGetSut : public ToyBacked {
public:
    std::optional<std::string> get(std::string_view p) const override {
        if (!p.empty()) {
            throw std::runtime_error("get threw");
        }
        return toy_.get(p);
    }
};

TEST(PointerValueLawTest, PassesOnTheToyAndIsEvaluated) {
    ToySut sut;
    const LawResult r
        = check_pointer_values(sut, R"({"a/b":{"c~d":[1,2.5,"x"]},"e":[[],{}],"f":null})", kCfg);
    EXPECT_TRUE(r.evaluated) << r.skip_reason;
    EXPECT_TRUE(r.violations.empty()) << r.violations.front().message;
}

TEST(PointerValueLawTest, ReportsAGetThatReturnsASiblingMember) {
    SiblingGetterSut sut;
    const LawResult r = check_pointer_values(sut, R"({"a":1,"b":2})", kCfg);
    const OracleViolation* v = first_of(r.violations, Law::O3);
    ASSERT_NE(v, nullptr) << "a get() returning the wrong (valid) member must be an O3 violation";
    EXPECT_EQ(v->pointer, "/a");
    EXPECT_EQ(v->expected, "1");
    EXPECT_EQ(v->actual, "2");
    EXPECT_EQ(v->input, R"({"a":1,"b":2})");
}

TEST(PointerValueLawTest, SkipsADocumentTheReferenceCannotModel) {
    ToySut sut; // the toy accepts 1e309; the reference reports an overflow
    const LawResult r = check_pointer_values(sut, "[1e309]", kCfg);
    EXPECT_FALSE(r.evaluated);
    EXPECT_FALSE(r.skip_reason.empty());
    EXPECT_TRUE(r.violations.empty());
}

TEST(PointerValueLawTest, PointersThroughADuplicatedKeyAreNotFindings) {
    // The toy keeps the FIRST "a", the reference the LAST: neither is wrong.
    ToySut sut;
    const LawResult r = check_pointer_values(sut, R"({"a":{"x":1},"a":{"x":2},"b":3})", kCfg);
    EXPECT_TRUE(r.evaluated);
    EXPECT_TRUE(r.violations.empty()) << r.violations.front().message;
}

TEST(PointerValueLawTest, ASutThrowOnThePropertyPathIsANamedViolation) {
    ThrowingGetSut sut;
    LawResult r;
    EXPECT_NO_THROW(r = check_pointer_values(sut, R"({"a":1})", kCfg));
    const OracleViolation* v = first_of(r.violations, Law::O3);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->pointer, "/a");
}

TEST(PointerValueLawTest, TheToyResolvesAnEscapedKeyByItsValue) {
    // The law's first finding against the toy: pointers() enumerated the RAW escaped key
    // body (/a\"b) where RFC 6901 names the decoded key (/a"b).
    ToySut sut;
    const LawResult r = check_pointer_values(sut, R"({"a\"b":1,"cA":[true]})", kCfg);
    EXPECT_TRUE(r.evaluated);
    EXPECT_TRUE(r.violations.empty()) << r.violations.front().message;
}

// ===========================================================================
// O4 — set-then-remove is the identity
// ===========================================================================

// remove() says Applied but leaves the value behind.
class StickyRemoveSut : public ToyBacked {
public:
    WriteResult remove(std::string_view) override { return {WriteResult::Status::Applied, ""}; }
};

// set() says Applied but writes nothing, so remove() has nothing to remove: the text comes
// back identical. Only "get(fresh) must equal v between the two" sees it.
class NoOpSetSut : public ToyBacked {
public:
    WriteResult set(std::string_view, std::string_view) override {
        return {WriteResult::Status::Applied, ""};
    }
    WriteResult remove(std::string_view) override { return {WriteResult::Status::Applied, ""}; }
};

// set() refuses a fresh pointer whose parent exists.
class RefusingSetSut : public ToyBacked {
public:
    WriteResult set(std::string_view, std::string_view) override {
        return {WriteResult::Status::Rejected, "no"};
    }
};

// A toy whose pointers() also names a parent that does not exist.
class PhantomParentSut : public ToyBacked {
public:
    std::vector<std::string> pointers(int d) const override {
        auto p = toy_.pointers(d);
        p.insert(p.begin(), "/nope");
        return p;
    }
};

class ThrowingRemoveSut : public ToyBacked {
public:
    WriteResult remove(std::string_view) override { throw std::runtime_error("remove threw"); }
};

TEST(SetRemoveLawTest, PassesOnTheToyAndIsEvaluated) {
    ToySut sut;
    const LawResult r = check_set_remove(sut, R"({"a":[1,{"b":[]}],"c":{}})", kCfg);
    EXPECT_TRUE(r.evaluated) << r.skip_reason;
    EXPECT_TRUE(r.violations.empty()) << r.violations.front().message;
}

TEST(SetRemoveLawTest, ReportsARemoveThatLeavesTheValueBehind) {
    StickyRemoveSut sut;
    const LawResult r = check_set_remove(sut, R"({"a":1})", kCfg);
    const OracleViolation* v = first_of(r.violations, Law::O4);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->expected, R"({"a":1})");
    EXPECT_NE(v->actual, R"({"a":1})");
    EXPECT_FALSE(v->pointer.empty());
}

TEST(SetRemoveLawTest, ReportsASetThatDoesNotTakeEvenThoughTheTextComesBack) {
    NoOpSetSut sut;
    const LawResult r = check_set_remove(sut, "[1]", kCfg);
    EXPECT_TRUE(has_law(r.violations, Law::O4));
}

TEST(SetRemoveLawTest, ReportsASetThatRefusesAFreshPointerUnderAnExistingParent) {
    RefusingSetSut sut;
    const LawResult r = check_set_remove(sut, "{}", kCfg);
    EXPECT_TRUE(has_law(r.violations, Law::O4));
}

TEST(SetRemoveLawTest, IsSkippedForASutWithNoWritePath) {
    // Implements only the four v0.1 verbs: set/remove keep the interface's Unsupported.
    class NoWrites : public SystemUnderTest {
    public:
        ParseResult parse(std::string_view t, const ParseConfig& c) override {
            return toy.parse(t, c);
        }
        std::string serialize() const override { return toy.serialize(); }
        std::vector<std::string> pointers(int d) const override { return toy.pointers(d); }
        std::optional<std::string> get(std::string_view p) const override { return toy.get(p); }
        ToySut toy;
    } sut;
    const LawResult r = check_set_remove(sut, R"({"a":1})", kCfg);
    EXPECT_FALSE(r.evaluated);
    EXPECT_FALSE(r.skip_reason.empty());
    EXPECT_TRUE(r.violations.empty());
}

TEST(SetRemoveLawTest, AScalarDocumentHasNoParentAndIsASkip) {
    ToySut sut;
    const LawResult r = check_set_remove(sut, "42", kCfg);
    EXPECT_FALSE(r.evaluated);
    EXPECT_TRUE(r.violations.empty());
}

TEST(SetRemoveLawTest, AParentThatDoesNotExistIsASkipNotAFinding) {
    PhantomParentSut sut;
    const LawResult r = check_set_remove(sut, R"({"a":1})", kCfg);
    EXPECT_TRUE(r.evaluated); // the root still is a real parent
    EXPECT_TRUE(r.violations.empty()) << r.violations.front().message;
}

TEST(SetRemoveLawTest, ASutThrowOnThePropertyPathIsANamedViolation) {
    ThrowingRemoveSut sut;
    LawResult r;
    EXPECT_NO_THROW(r = check_set_remove(sut, R"({"a":1})", kCfg));
    EXPECT_TRUE(has_law(r.violations, Law::O4));
}

// ===========================================================================
// O5 — formatter-preset agreement
// ===========================================================================

// The pretty mode drops the last array element: valid JSON, a different document.
class LossyPrettySut : public ToyBacked {
public:
    std::string serialize_mode(int m) const override {
        std::string s = toy_.serialize_mode(m);
        if (m == 1) {
            const auto comma = s.rfind(',');
            const auto close = s.rfind(']');
            if (comma != std::string::npos && close != std::string::npos && comma < close) {
                s.erase(comma, close - comma);
            }
        }
        return s;
    }
};

// The pretty mode is not byte-stable: every call indents one more space.
class DriftingPrettySut : public ToyBacked {
public:
    std::string serialize_mode(int m) const override {
        if (m == 1) {
            ++calls_;
            return std::string(static_cast<size_t>(calls_), ' ') + toy_.serialize_mode(1);
        }
        return toy_.serialize_mode(m);
    }

private:
    mutable int calls_ = 0;
};

class ThrowingModeSut : public ToyBacked {
public:
    std::string serialize_mode(int) const override { throw std::runtime_error("mode threw"); }
};

TEST(ModeAgreementLawTest, PassesOnTheToyAndIsEvaluated) {
    ToySut sut;
    const LawResult r = check_mode_agreement(sut, R"({"a":[1,{"b":[]}],"c":"x"})", kCfg);
    EXPECT_TRUE(r.evaluated) << r.skip_reason;
    EXPECT_TRUE(r.violations.empty()) << r.violations.front().message;
}

TEST(ModeAgreementLawTest, ReportsAPrettyModeThatDisagreesWithCompact) {
    LossyPrettySut sut;
    const LawResult r = check_mode_agreement(sut, "[1,2,3]", kCfg);
    const OracleViolation* v = first_of(r.violations, Law::O5);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->expected, "[1,2,3]");
    EXPECT_EQ(v->actual, "[1,2]");
}

TEST(ModeAgreementLawTest, ReportsAModeThatIsNotByteStable) {
    DriftingPrettySut sut;
    const LawResult r = check_mode_agreement(sut, "[1]", kCfg);
    EXPECT_TRUE(has_law(r.violations, Law::O5));
}

TEST(ModeAgreementLawTest, IsSkippedForASingleModeSut) {
    class OneMode : public ToyBacked {
    public:
        int serialize_mode_count() const override { return 1; }
    } sut;
    const LawResult r = check_mode_agreement(sut, "[1]", kCfg);
    EXPECT_FALSE(r.evaluated);
    EXPECT_TRUE(r.violations.empty());
}

TEST(ModeAgreementLawTest, ASutThrowOnThePropertyPathIsANamedViolation) {
    ThrowingModeSut sut;
    LawResult r;
    EXPECT_NO_THROW(r = check_mode_agreement(sut, "[1]", kCfg));
    EXPECT_TRUE(has_law(r.violations, Law::O5));
}

// ===========================================================================
// O6 — lazy-number text preservation (conditional)
// ===========================================================================

// Declares the promise, then rewrites "-0" to "0" — the same number, a different text, so
// O1 (stable), O3 (numerically equal) and O5 all pass it.
class NegativeZeroRewriterSut : public ToyBacked {
public:
    std::string serialize() const override {
        std::string s = toy_.serialize();
        for (auto pos = s.find("-0"); pos != std::string::npos; pos = s.find("-0", pos)) {
            const bool more = pos + 2 < s.size() && (s[pos + 2] == '.' || s[pos + 2] == 'e');
            if (!more) {
                s.erase(pos, 1);
            } else {
                pos += 2;
            }
        }
        return s;
    }
};

class UndeclaredRewriterSut : public NegativeZeroRewriterSut {
public:
    bool preserves_number_text() const override { return false; }
};

class ThrowingSerializeAfterReparseSut : public ToyBacked {
public:
    std::string serialize() const override {
        if (++calls_ > 1) {
            throw std::runtime_error("serialize threw");
        }
        return toy_.serialize();
    }

private:
    mutable int calls_ = 0;
};

TEST(NumberTextLawTest, PassesOnTheToyAndIsEvaluated) {
    ToySut sut;
    const LawResult r = check_number_text(sut, R"({"a":[-0,1E5,1.50,-0.0e-0],"b":"-0 1"})", kCfg);
    EXPECT_TRUE(r.evaluated) << r.skip_reason;
    EXPECT_TRUE(r.violations.empty()) << r.violations.front().message;
}

TEST(NumberTextLawTest, ReportsASerializerThatRewritesANumbersText) {
    NegativeZeroRewriterSut sut;
    const LawResult r = check_number_text(sut, "[1,-0]", kCfg);
    const OracleViolation* v = first_of(r.violations, Law::O6);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->expected, "-0");
    EXPECT_EQ(v->actual, "[1,0]");
}

TEST(NumberTextLawTest, AnUndeclaredSutIsNotFailedForIt) {
    UndeclaredRewriterSut sut;
    const LawResult r = check_number_text(sut, "[1,-0]", kCfg);
    EXPECT_FALSE(r.evaluated);
    EXPECT_FALSE(r.skip_reason.empty());
    EXPECT_TRUE(r.violations.empty());
}

TEST(NumberTextLawTest, ADuplicatedKeyIsASkip) {
    ToySut sut;
    const LawResult r = check_number_text(sut, R"({"a":1,"a":2})", kCfg);
    EXPECT_FALSE(r.evaluated);
}

TEST(NumberTextLawTest, ASutThrowOnThePropertyPathIsANamedViolation) {
    ThrowingSerializeAfterReparseSut sut;
    LawResult r;
    EXPECT_NO_THROW(r = check_number_text(sut, "[1]", kCfg));
    EXPECT_TRUE(has_law(r.violations, Law::O6));
}

// ===========================================================================
// O7 — strict accepts are a subset of lazy accepts
// ===========================================================================

// The lazy configuration rejects an exponent the strict one accepts.
class LazyRejectsExponentSut : public ToyBacked {
public:
    ParseResult parse(std::string_view t, const ParseConfig& c) override {
        if (!c.strict_numbers && t.find_first_of("eE") != std::string_view::npos) {
            return {false, "lazy: no exponents"};
        }
        return toy_.parse(t, c);
    }
};

// Both accept, but the lazy canonical text differs.
class LazyCanonicalDiffersSut : public ToyBacked {
public:
    ParseResult parse(std::string_view t, const ParseConfig& c) override {
        lazy_ = !c.strict_numbers;
        return toy_.parse(t, c);
    }
    std::string serialize() const override { return toy_.serialize() + (lazy_ ? " " : ""); }

private:
    bool lazy_ = false;
};

// Strict REJECTS, lazy accepts: the documented extension, never a finding.
class StrictRejectsSut : public ToyBacked {
public:
    ParseResult parse(std::string_view t, const ParseConfig& c) override {
        if (c.strict_numbers) {
            return {false, "strict says no"};
        }
        return toy_.parse(t, c);
    }
};

class ThrowingLazyParseSut : public ToyBacked {
public:
    ParseResult parse(std::string_view t, const ParseConfig& c) override {
        if (!c.strict_numbers) {
            throw std::runtime_error("lazy parse threw");
        }
        return toy_.parse(t, c);
    }
};

TEST(StrictSubsetLawTest, PassesOnTheToyAndIsEvaluated) {
    ToySut sut;
    const LawResult r = check_strict_subset(sut, R"({"a":[1e5,-0.25]})", kCfg);
    EXPECT_TRUE(r.evaluated) << r.skip_reason;
    EXPECT_TRUE(r.violations.empty()) << r.violations.front().message;
}

TEST(StrictSubsetLawTest, ReportsAStrictAcceptThatLazyRejects) {
    LazyRejectsExponentSut sut;
    const LawResult r = check_strict_subset(sut, "[1e5]", kCfg);
    const OracleViolation* v = first_of(r.violations, Law::O7);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->input, "[1e5]");
}

TEST(StrictSubsetLawTest, ReportsACanonicalTextThatDiffersBetweenTheTwo) {
    LazyCanonicalDiffersSut sut;
    const LawResult r = check_strict_subset(sut, "[1]", kCfg);
    EXPECT_TRUE(has_law(r.violations, Law::O7));
}

TEST(StrictSubsetLawTest, TheReverseDirectionIsNotAFinding) {
    StrictRejectsSut sut;
    const LawResult r = check_strict_subset(sut, "[1]", kCfg);
    EXPECT_FALSE(r.evaluated);
    EXPECT_TRUE(r.violations.empty());
}

TEST(StrictSubsetLawTest, ASutThrowOnThePropertyPathIsANamedViolation) {
    ThrowingLazyParseSut sut;
    LawResult r;
    EXPECT_NO_THROW(r = check_strict_subset(sut, "[1]", kCfg));
    EXPECT_TRUE(has_law(r.violations, Law::O7));
}

// ===========================================================================
// check_all — every law, one call
// ===========================================================================

TEST(CheckAllTest, EveryLawIsEvaluatedAndCleanOnTheToy) {
    ToySut sut;
    const auto report = check_all(sut, R"({"a":[1,-0,{"b~/":"x"}],"c":2.5e3})", kCfg);
    for (size_t i = 0; i < kLawCount; ++i) {
        EXPECT_TRUE(report.evaluated[i]) << "law index " << i << " was not evaluated";
    }
    EXPECT_TRUE(report.violations.empty()) << report.violations.front().message;
}

TEST(CheckAllTest, NamesTheLawThatCaughtTheDefect) {
    SiblingGetterSut sut;
    const auto report = check_all(sut, R"({"a":1,"b":2})", kCfg);
    EXPECT_TRUE(has_law(report.violations, Law::O3));
    EXPECT_FALSE(has_law(report.violations, Law::O1));
    EXPECT_FALSE(has_law(report.violations, Law::O2)) << "valid JSON: O2 is blind to it";
    EXPECT_TRUE(report.evaluated[law_index(Law::O3)]);
}

TEST(CheckAllTest, LawNamesAreDistinctAndCarryTheLawId) {
    EXPECT_EQ(jsonfuzz::law_name(Law::O1).substr(0, 2), "O1");
    EXPECT_EQ(jsonfuzz::law_name(Law::O3).substr(0, 2), "O3");
    EXPECT_EQ(jsonfuzz::law_name(Law::O7).substr(0, 2), "O7");
    EXPECT_NE(jsonfuzz::law_name(Law::O4), jsonfuzz::law_name(Law::O5));
}

TEST(CheckAllTest, RejectionLeavesEveryLawUnevaluated) {
    ToySut sut;
    const auto report = check_all(sut, "not json", kCfg);
    for (size_t i = 0; i < kLawCount; ++i) {
        EXPECT_FALSE(report.evaluated[i]);
    }
    EXPECT_TRUE(report.violations.empty());
}

// A caller must never have to re-run a law to explain why it did not run: the report carries the
// reason each law gave for skipping. Re-running would invoke the SUT a second time behind the
// caller's back, and would put a second copy of the law-to-check mapping outside the oracle.
TEST(CheckAllTest, EverySkippedLawCarriesItsReasonAndEveryEvaluatedLawIsSilent) {
    ToySut sut;
    const auto report = check_all(sut, "[1e309]", kCfg);
    for (size_t i = 0; i < kLawCount; ++i) {
        if (report.evaluated[i]) {
            EXPECT_TRUE(report.skip_reasons[i].empty())
                << "law " << i << " ran but still carries a reason: " << report.skip_reasons[i];
        } else {
            EXPECT_FALSE(report.skip_reasons[i].empty())
                << "law " << i << " did not run and does not say why";
        }
    }
    EXPECT_FALSE(report.evaluated[law_index(OracleViolation::Law::O3)]);
    EXPECT_FALSE(report.evaluated[law_index(OracleViolation::Law::O4)]);
    EXPECT_EQ(report.skip_reasons[law_index(OracleViolation::Law::O3)],
              "the reference cannot model the canonical text");
}

} // namespace
