#pragma once

/// JSONFuzz JSON-aware mutation operators (brief §3).
///
/// One operator per bug class. Each is documented with the bug class it targets. The
/// operators may use nlohmann/json as the toolkit's own model internally; no JSON parser
/// is written in this repo. The model is ours, the SUT is theirs.
///
/// Mutating an input that does not parse returns the input unchanged and never throws.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jsonfuzz {

/// The mutation operators. Each targets a distinct bug class.
enum class Op : std::uint8_t {
    TruncateAtValue,     // cut the document at a value boundary
    TruncateAtKey,       // cut the document at a key boundary
    TruncateAtStructure, // cut the document at a structure boundary
    TruncateAtEscape,    // cut the document at an escape boundary
    DeleteMember,        // remove an object member
    DeleteElement,       // remove an array element
    DuplicateSubtree,    // duplicate a subtree
    WrapInArray,         // wrap the whole document in an array
    WrapInObject,        // wrap the whole document in an object
    ChangeValueKind,     // change a value's kind (number <-> string, etc.)
    BreakNumberGrammar,  // break a number's grammar (-01, 1.0., 2.e+3)
    AdversarialEscape,   // replace a string with an adversarial escape
    UnbalanceBrackets,   // remove a bracket
    MismatchCloser,      // change a closer to a different one
    InjectInvalidUtf8,   // inject invalid UTF-8 bytes
    SwapMembers,         // swap two object members
};

/// The human-readable name of an operator (for --list-ops).
std::string_view name(Op op);

/// Every operator, in declaration order (for --list-ops).
std::vector<Op> all_ops();

/// The result of applying one operator.
struct MutateResult {
    std::string text;
    bool changed = false;
};

/// Apply one operator to `input` with a deterministic `seed`. If `input` does not parse,
/// returns it unchanged (changed == false) and never throws.
MutateResult apply(std::string_view input, Op op, uint64_t seed);

/// The libFuzzer-shaped entry point: mutate `data[0..size)` in place, writing at most
/// `max_size` bytes, deterministically from `seed`. Returns the new size. If the input
/// does not parse, it is returned unchanged.
size_t mutate(uint8_t* data, size_t size, size_t max_size, unsigned seed);

} // namespace jsonfuzz
