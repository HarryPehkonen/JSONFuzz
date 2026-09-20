#pragma once

/// JSONFuzz structure-aware generator (brief §2).
///
/// Deterministic generation from a bounded byte stream: the same bytes always produce the
/// same document, and the document is produced WITH intent — the intent record is the
/// oracle's independent truth for the fixed-point oracles, so no reference parser is
/// needed for them.
///
/// TOTALNESS: any byte stream produces a document and generation never fails. When the
/// byte source is exhausted, the generator reads a deterministic 0 and keeps going, so an
/// empty stream still yields a valid document. A generator that can fail is a fuzzer that
/// reports its own bugs as findings.

#include <cstddef>
#include <cstdint>
#include <string>

namespace jsonfuzz {

/// Generation options. All weights are 0..100.
struct GenOptions {
    int max_depth = 8;
    int max_members = 8;
    int max_string = 32;
    int nesting_bias = 0;       // higher => deeper nesting (more containers)
    int adversarial_weight = 0; // higher => more adversarial elements
};

/// The intent record: what the generator decided to produce. This is the oracle's
/// independent truth for member counts and kinds — it must agree with the text.
struct Intent {
    int object_count = 0;
    int array_count = 0;
    int string_count = 0;
    int number_count = 0;
    int bool_count = 0;
    int null_count = 0;
    int max_depth_reached = 0;
    int total_members = 0; // object members + array elements
    bool has_duplicate_key = false;
    bool has_empty_key = false;
    bool has_pointer_key = false; // a key holding '/' or '~'
    bool has_lone_surrogate = false;
    bool has_control_char = false;
    bool has_boundary_number = false;
};

/// A generated document: the text plus the intent that produced it.
struct Generated {
    std::string text;
    Intent intent;
};

/// A bounded, deterministic byte reader — the same stream libFuzzer hands us. When
/// exhausted, next() returns 0 so the generator stays total.
class ByteSource {
public:
    ByteSource(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    uint8_t next() { return pos_ < size_ ? data_[pos_++] : 0; }
    bool empty() const { return pos_ >= size_; }
    size_t remaining() const { return size_ - pos_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

/// Generate a document from the byte stream. Never fails; the same bytes give the same
/// document.
Generated generate(const GenOptions& options, ByteSource& bytes);

} // namespace jsonfuzz
