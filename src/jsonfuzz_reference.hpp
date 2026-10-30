#pragma once

/// The reference's view of numbers it cannot hold (private to jsonfuzz_core; no nlohmann here).
///
/// nlohmann — the toolkit's own model and the reference of O3/O4 and of reading 3 — throws
/// out_of_range.406 on any JSON number a double cannot hold (1e309, -1e309, a 400-digit
/// integer), so on a document holding one the whole reference was missing and the laws that
/// depend on it examined nothing. Before the reference parses a text, every such number token
/// is rewritten into a JSON string that holds kNumberTag followed by the token's exact text.
/// nlohmann then models the document, and two such numbers are equal exactly when their texts
/// are. Numbers a double CAN hold are left untouched, so they still compare numerically.
///
/// The tag starts with U+0000, which a JSON string can only spell as the escape \u0000. A
/// caller must still count the tagged strings in the parsed tree against `tagged`: a document
/// that already holds a string spelling the tag is ambiguous, and must not be compared.

#include <string>
#include <string_view>

namespace jsonfuzz::detail {

inline constexpr char kNumberTagBody[] = "jsonfuzz-number:";
/// "\0jsonfuzz-number:" — U+0000 followed by the body (std::string_view keeps the NUL).
inline constexpr std::string_view kNumberTag{"\0jsonfuzz-number:", sizeof(kNumberTagBody)};

struct TaggedText {
    std::string text;
    int tagged = 0; // how many number tokens were rewritten
};

/// Rewrite every number token outside a string whose value overflows a double (strtod gives
/// ±inf, the test nlohmann applies) into "\u0000jsonfuzz-number:<token>". Any other byte is
/// copied unchanged, so invalid text stays invalid.
TaggedText tag_unmodelable_numbers(std::string_view json_text);

/// The display form of a reference dump: each tagged string goes back to its number text.
std::string untag_numbers(std::string_view dumped);

} // namespace jsonfuzz::detail
