#pragma once

/// JSONFuzz parser-agnostic SystemUnderTest interface (brief §1).
///
/// Strings cross the boundary; no parser type is shared. A real parser (JSOM, in a
/// later brief) is adapted by implementing this interface in THAT parser's repo —
/// JSONFuzz never depends on any parser, and builds and tests with no adapter present.
///
/// The toy SUT in this header is the deliberately-correct minimal model this repo's own
/// tests and fuzz target use before a real adapter exists.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jsonfuzz {

/// Parse options a SUT may honour. The toy SUT honours max_depth only: its number grammar
/// is always strict, so it answers strict_numbers = true and = false identically (which
/// trivially satisfies the strict-subset-of-lazy law, O7). The others are carried so a
/// real adapter can read them.
struct ParseConfig {
    bool allow_comments = false;
    bool unicode_escapes = false;
    bool strict_numbers = false;
    int max_depth = 256;
};

/// The outcome of a parse. REJECTION IS AN OUTCOME, NOT A THROW: a SUT must never throw
/// to report that an input is invalid, and the oracles run the property OUTSIDE every
/// catch (a broad catch around the property turns a wrong answer into "expected for
/// invalid input" — the exact way JSOM's harness missed a serializer that emits invalid
/// JSON).
struct ParseResult {
    bool accepted = false;
    std::string error;
};

/// The outcome of a write verb (set / remove). Same discipline as ParseResult: a refusal is
/// an outcome, never a throw.
///   Applied     — the model was changed.
///   Rejected    — the SUT refused this pointer (e.g. its parent does not exist).
///   Unsupported — the SUT has no write path at all; the write-path law (O4) is skipped
///                 for it rather than failed. This is the default for every SUT.
struct WriteResult {
    enum class Status : std::uint8_t { Applied, Rejected, Unsupported };
    Status status = Status::Unsupported;
    std::string error;
};

/// One adapter per parser. Lives in THAT parser's repo; JSONFuzz only ever sees this
/// interface.
class SystemUnderTest {
public:
    virtual ~SystemUnderTest() = default;

    /// Parse `text`; on acceptance the SUT holds the canonical model that serialize(),
    /// pointers() and get() read. Rejection is a ParseResult, never a throw.
    virtual ParseResult parse(std::string_view text, const ParseConfig& config) = 0;

    /// Canonical text of the last accepted parse. Empty if nothing has been accepted.
    virtual std::string serialize() const = 0;

    /// RFC 6901 pointers into the last accepted parse, bounded by max_depth.
    virtual std::vector<std::string> pointers(int max_depth) const = 0;

    /// The value at `pointer`, serialized as JSON. nullopt when the pointer does not
    /// resolve (or nothing has been accepted).
    virtual std::optional<std::string> get(std::string_view pointer) const = 0;

    // ---- v0.3 OPTIONAL capabilities. Each default declares "not offered", so a SUT that
    // ---- does not implement one is skipped by the law that needs it, never failed by it.

    /// Write `value_json` (a JSON text) at `pointer` in the last accepted parse: an object
    /// member is added or replaced, an array index equal to the size (or "-") appends, a
    /// smaller index replaces. Never throws. Default: Unsupported.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    virtual WriteResult set(std::string_view pointer, std::string_view value_json) {
        (void)pointer;
        (void)value_json;
        return {WriteResult::Status::Unsupported, "set() is not offered by this SUT"};
    }

    /// Remove the value at `pointer` (an object member or an array element) from the last
    /// accepted parse. Never throws. Default: Unsupported.
    virtual WriteResult remove(std::string_view pointer) {
        (void)pointer;
        return {WriteResult::Status::Unsupported, "remove() is not offered by this SUT"};
    }

    /// How many serialization modes (formatter presets) the SUT offers. Modes are numbered
    /// 0 .. serialize_mode_count() - 1. JSOM has five presets; the toy has two (compact,
    /// pretty). Default: 1 — a single mode, so the preset-agreement law (O5) is skipped.
    virtual int serialize_mode_count() const { return 1; }

    /// The last accepted parse, serialized in mode `mode`. Default: mode 0 is serialize(),
    /// any other mode is empty.
    virtual std::string serialize_mode(int mode) const {
        return mode == 0 ? serialize() : std::string();
    }

    /// Does the SUT PROMISE that a number's original text survives a round trip (JSOM's
    /// LazyNumber does)? Only a SUT that declares it is held to the number-text law (O6).
    /// Default: false.
    virtual bool preserves_number_text() const { return false; }
};

/// Resolves a SUT by name for the CLI (`--sut <name>`). An unknown name is a documented
/// error, not a crash: create() returns nullptr and the caller reports it.
class SutRegistry {
public:
    using Factory = std::function<std::unique_ptr<SystemUnderTest>()>;

    void register_sut(std::string name, Factory factory) {
        factories_[std::move(name)] = std::move(factory);
    }

    /// nullptr for an unknown name — the caller decides how to report it.
    std::unique_ptr<SystemUnderTest> create(std::string_view name) const {
        auto it = factories_.find(std::string(name));
        if (it == factories_.end()) {
            return nullptr;
        }
        return it->second();
    }

    bool contains(std::string_view name) const { return factories_.count(std::string(name)) > 0; }

    std::vector<std::string> names() const {
        std::vector<std::string> out;
        out.reserve(factories_.size());
        for (const auto& kv : factories_) {
            out.push_back(kv.first);
        }
        return out;
    }

private:
    std::map<std::string, Factory> factories_;
};

/// The process-wide registry. The toy SUT is registered here as "toy" by the library.
SutRegistry& default_registry();

/// The deliberately-correct toy SUT: a minimal std::variant-style model that parses,
/// serializes, enumerates RFC 6901 pointers and resolves get(). It is strict (rejects -01,
/// 1.0., 2.e+3, raw control characters) so the repo can be fuzzed against a correct
/// reference before a real adapter exists. It implements every v0.3 capability so every
/// v0.3 law is exercised: set/remove, two serialization modes (0 compact, 1 pretty) and
/// the number-text promise (numbers are stored as their raw token).
class ToySut : public SystemUnderTest {
public:
    ToySut();
    ~ToySut() override;
    ParseResult parse(std::string_view text, const ParseConfig& config) override;
    std::string serialize() const override;
    std::vector<std::string> pointers(int max_depth) const override;
    std::optional<std::string> get(std::string_view pointer) const override;
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    WriteResult set(std::string_view pointer, std::string_view value_json) override;
    WriteResult remove(std::string_view pointer) override;
    int serialize_mode_count() const override;
    std::string serialize_mode(int mode) const override;
    bool preserves_number_text() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// A deliberately-broken SUT, registered as "broken-comma": it parses strictly (via the
/// toy) but serializes the last accepted parse with a trailing comma before a closer, so
/// its serializer output does not reparse. NOT used by the fuzz target (which needs a
/// correct reference); it exists so the CLI `check` verb and the oracle's teeth can be
/// exercised end-to-end against a known-bad adapter.
class BrokenCommaSut : public SystemUnderTest {
public:
    BrokenCommaSut();
    ~BrokenCommaSut() override;
    ParseResult parse(std::string_view text, const ParseConfig& config) override;
    std::string serialize() const override;
    std::vector<std::string> pointers(int max_depth) const override;
    std::optional<std::string> get(std::string_view pointer) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jsonfuzz
