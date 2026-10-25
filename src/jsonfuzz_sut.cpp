#include <jsonfuzz/sut.hpp>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <set>
#include <utility>

namespace jsonfuzz {

namespace {

// The toy SUT's model: a minimal std::variant-style JSON value. Strings and numbers are
// stored as their RAW text (the escaped body / the number token) so serialize() re-emits
// them byte-for-byte — that is what makes the round-trip oracle hold on the toy.
struct JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;

struct JsonValue {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    std::string number;
    std::string str;
    JsonArray array;
    JsonObject object;
};

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// A strict recursive-descent parser. Rejection is a bool + error string, never a throw.
class Parser {
public:
    Parser(std::string_view text, const ParseConfig& config) : text_(text), config_(config) {}

    bool parse(JsonValue& out) {
        skip_ws();
        if (!parse_value(out, 0)) {
            return false;
        }
        skip_ws();
        if (pos_ != text_.size()) {
            error_ = "trailing content after the document";
            return false;
        }
        return true;
    }

    const std::string& error() const { return error_; }

private:
    std::string_view text_;
    const ParseConfig& config_;
    size_t pos_ = 0;
    std::string error_;

    void skip_ws() {
        while (pos_ < text_.size()
               && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n'
                   || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool parse_value(JsonValue& out, int depth) {
        if (depth > config_.max_depth) {
            error_ = "nesting exceeds max_depth";
            return false;
        }
        skip_ws();
        if (pos_ >= text_.size()) {
            error_ = "unexpected end of input";
            return false;
        }
        const char c = text_[pos_];
        switch (c) {
        case 'n':
            return parse_literal("null", out, JsonValue::Kind::Null);
        case 't':
            return parse_literal("true", out, JsonValue::Kind::Bool, true);
        case 'f':
            return parse_literal("false", out, JsonValue::Kind::Bool, false);
        case '"': {
            std::string s;
            if (!parse_string(s)) {
                return false;
            }
            out.kind = JsonValue::Kind::String;
            out.str = std::move(s);
            return true;
        }
        case '[':
            return parse_array(out, depth);
        case '{':
            return parse_object(out, depth);
        default:
            if (c == '-' || is_digit(c)) {
                std::string n;
                if (!parse_number(n)) {
                    return false;
                }
                out.kind = JsonValue::Kind::Number;
                out.number = std::move(n);
                return true;
            }
            error_ = "unexpected character";
            return false;
        }
    }

    bool parse_literal(const char* lit, JsonValue& out, JsonValue::Kind kind,
                       bool boolean = false) {
        const size_t len = std::char_traits<char>::length(lit);
        if (text_.substr(pos_, len) != std::string_view(lit, len)) {
            error_ = "malformed literal";
            return false;
        }
        pos_ += len;
        out.kind = kind;
        out.boolean = boolean;
        return true;
    }

    bool parse_string(std::string& out) {
        if (pos_ >= text_.size() || text_[pos_] != '"') {
            error_ = "expected a string";
            return false;
        }
        ++pos_;
        const size_t start = pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == '"') {
                out = std::string(text_.substr(start, pos_ - start));
                ++pos_;
                return true;
            }
            if (c == '\\') {
                ++pos_;
                if (pos_ >= text_.size()) {
                    error_ = "unterminated escape";
                    return false;
                }
                const char e = text_[pos_];
                switch (e) {
                case '"':
                case '\\':
                case '/':
                case 'b':
                case 'f':
                case 'n':
                case 'r':
                case 't':
                    ++pos_;
                    break;
                case 'u':
                    ++pos_;
                    if (pos_ + 4 > text_.size()) {
                        error_ = "truncated unicode escape";
                        return false;
                    }
                    for (int i = 0; i < 4; ++i) {
                        if (!is_hex(text_[pos_ + i])) {
                            error_ = "bad unicode escape";
                            return false;
                        }
                    }
                    pos_ += 4;
                    break;
                default:
                    error_ = "invalid escape";
                    return false;
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                error_ = "raw control character in string";
                return false;
            }
            ++pos_;
        }
        error_ = "unterminated string";
        return false;
    }

    bool parse_number(std::string& out) {
        const size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ >= text_.size() || !is_digit(text_[pos_])) {
            error_ = "number has no integer part";
            return false;
        }
        if (text_[pos_] == '0') {
            ++pos_;
        } else {
            while (pos_ < text_.size() && is_digit(text_[pos_])) {
                ++pos_;
            }
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (pos_ >= text_.size() || !is_digit(text_[pos_])) {
                error_ = "number has no fraction digits";
                return false;
            }
            while (pos_ < text_.size() && is_digit(text_[pos_])) {
                ++pos_;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= text_.size() || !is_digit(text_[pos_])) {
                error_ = "number has no exponent digits";
                return false;
            }
            while (pos_ < text_.size() && is_digit(text_[pos_])) {
                ++pos_;
            }
        }
        out = std::string(text_.substr(start, pos_ - start));
        return true;
    }

    bool parse_array(JsonValue& out, int depth) {
        ++pos_;
        out.kind = JsonValue::Kind::Array;
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_ws();
            JsonValue v;
            if (!parse_value(v, depth + 1)) {
                return false;
            }
            out.array.push_back(std::move(v));
            skip_ws();
            if (pos_ >= text_.size()) {
                error_ = "unterminated array";
                return false;
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            error_ = "expected ',' or ']'";
            return false;
        }
    }

    bool parse_object(JsonValue& out, int depth) {
        ++pos_;
        out.kind = JsonValue::Kind::Object;
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                error_ = "expected ':' after object key";
                return false;
            }
            ++pos_;
            JsonValue v;
            if (!parse_value(v, depth + 1)) {
                return false;
            }
            out.object.emplace_back(std::move(key), std::move(v));
            skip_ws();
            if (pos_ >= text_.size()) {
                error_ = "unterminated object";
                return false;
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            error_ = "expected ',' or '}'";
            return false;
        }
    }
};

void serialize_value(const JsonValue& v, std::string& out) {
    switch (v.kind) {
    case JsonValue::Kind::Null:
        out += "null";
        break;
    case JsonValue::Kind::Bool:
        out += v.boolean ? "true" : "false";
        break;
    case JsonValue::Kind::Number:
        out += v.number;
        break;
    case JsonValue::Kind::String:
        out += '"';
        out += v.str;
        out += '"';
        break;
    case JsonValue::Kind::Array:
        out += '[';
        for (size_t i = 0; i < v.array.size(); ++i) {
            if (i != 0) {
                out += ',';
            }
            serialize_value(v.array[i], out);
        }
        out += ']';
        break;
    case JsonValue::Kind::Object:
        out += '{';
        for (size_t i = 0; i < v.object.size(); ++i) {
            if (i != 0) {
                out += ',';
            }
            out += '"';
            out += v.object[i].first;
            out += "\":";
            serialize_value(v.object[i].second, out);
        }
        out += '}';
        break;
    }
}

std::string escape_pointer_token(const std::string& token) {
    std::string out;
    out.reserve(token.size());
    for (char c : token) {
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

std::string unescape_pointer_token(const std::string& token) {
    std::string out;
    out.reserve(token.size());
    for (size_t i = 0; i < token.size(); ++i) {
        if (token[i] == '~' && i + 1 < token.size()) {
            if (token[i + 1] == '0') {
                out += '~';
                ++i;
            } else if (token[i + 1] == '1') {
                out += '/';
                ++i;
            } else {
                out += token[i];
            }
        } else {
            out += token[i];
        }
    }
    return out;
}

// The VALUE of a key: its raw (escaped) body decoded to UTF-8. RFC 6901 tokens name this,
// not the escaped text — found by the v0.3 pointer-value law (O3) on its first run: the toy
// enumerated /a\"b for the key written "a\"b" (the value a"b, pointer /a"b), and the
// independent walk had no member there. A lone surrogate is encoded as its own three
// bytes (the reference rejects such a document anyway, so O3 skips it).
void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

uint32_t hex4(const std::string& s, size_t at) {
    uint32_t v = 0;
    for (size_t k = at; k < at + 4; ++k) {
        const char c = s[k];
        v <<= 4;
        if (is_digit(c)) {
            v |= static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= static_cast<uint32_t>(c - 'a' + 10);
        } else {
            v |= static_cast<uint32_t>(c - 'A' + 10);
        }
    }
    return v;
}

// `raw` was validated by the Parser (or written by encode_key), so every escape is whole.
std::string decode_key(const std::string& raw) {
    if (raw.find('\\') == std::string::npos) {
        return raw;
    }
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\\' || i + 1 >= raw.size()) {
            out += raw[i];
            continue;
        }
        const char e = raw[++i];
        switch (e) {
        case 'b':
            out += '\b';
            break;
        case 'f':
            out += '\f';
            break;
        case 'n':
            out += '\n';
            break;
        case 'r':
            out += '\r';
            break;
        case 't':
            out += '\t';
            break;
        case 'u': {
            uint32_t cp = hex4(raw, i + 1);
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < raw.size() && raw[i + 1] == '\\'
                && raw[i + 2] == 'u') {
                const uint32_t lo = hex4(raw, i + 3);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            append_utf8(out, cp);
            break;
        }
        default: // '"', '\\', '/'
            out += e;
            break;
        }
    }
    return out;
}

void collect_pointers(const JsonValue& v, const std::string& ptr, int depth, int max_depth,
                      std::vector<std::string>& out) {
    out.push_back(ptr);
    if (depth >= max_depth) {
        return;
    }
    if (v.kind == JsonValue::Kind::Array) {
        for (size_t i = 0; i < v.array.size(); ++i) {
            collect_pointers(v.array[i], ptr + "/" + std::to_string(i), depth + 1, max_depth, out);
        }
    } else if (v.kind == JsonValue::Kind::Object) {
        // get() resolves a duplicate key to its FIRST occurrence, so pointers() must
        // enumerate each key once or it emits pointers get() cannot resolve (measured:
        // the fuzzer found "//" — a nested empty-key member — unresolvable because the
        // first "" member was a scalar, not the object). First occurrence wins, matching
        // get().
        std::set<std::string> seen;
        for (const auto& kv : v.object) {
            std::string key = decode_key(kv.first);
            if (!seen.insert(key).second) {
                continue;
            }
            collect_pointers(kv.second, ptr + "/" + escape_pointer_token(key), depth + 1, max_depth,
                             out);
        }
    }
}

bool all_digits(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    for (char c : s) {
        if (!is_digit(c)) {
            return false;
        }
    }
    return true;
}

// Split an RFC 6901 pointer into its unescaped tokens. False when it is not a pointer.
bool split_pointer(std::string_view pointer, std::vector<std::string>& parts) {
    size_t i = 0;
    while (i < pointer.size()) {
        if (pointer[i] != '/') {
            return false;
        }
        ++i;
        size_t j = pointer.find('/', i);
        if (j == std::string_view::npos) {
            j = pointer.size();
        }
        parts.push_back(unescape_pointer_token(std::string(pointer.substr(i, j - i))));
        i = j;
    }
    return true;
}

bool parse_index(const std::string& part, size_t& idx) {
    if (!all_digits(part)) {
        return false;
    }
    idx = 0;
    for (char c : part) {
        idx = idx * 10 + static_cast<size_t>(c - '0');
    }
    return true;
}

// The member a key token names: the FIRST occurrence (by decoded key), as everywhere in
// the toy.
template <typename Object> auto find_member(Object& object, const std::string& key) {
    auto it = object.begin();
    for (; it != object.end(); ++it) {
        if (decode_key(it->first) == key) {
            break;
        }
    }
    return it;
}

// Walk the first `count` tokens of `parts` from `root`. nullptr when they do not resolve.
// A template so the read path (get) and the write path (set/remove) share one walk.
template <typename Value>
Value* resolve(Value& root, const std::vector<std::string>& parts, size_t count) {
    Value* cur = &root;
    for (size_t k = 0; k < count; ++k) {
        const std::string& part = parts[k];
        if (cur->kind == JsonValue::Kind::Array) {
            size_t idx = 0;
            if (!parse_index(part, idx) || idx >= cur->array.size()) {
                return nullptr;
            }
            cur = &cur->array[idx];
        } else if (cur->kind == JsonValue::Kind::Object) {
            auto it = find_member(cur->object, part);
            if (it == cur->object.end()) {
                return nullptr;
            }
            cur = &it->second;
        } else {
            return nullptr;
        }
    }
    return cur;
}

std::optional<std::string> get_value(const JsonValue& root, std::string_view pointer) {
    std::vector<std::string> parts;
    if (!split_pointer(pointer, parts)) {
        return std::nullopt;
    }
    const JsonValue* cur = resolve(root, parts, parts.size());
    if (cur == nullptr) {
        return std::nullopt;
    }
    std::string s;
    serialize_value(*cur, s);
    return s;
}

// A key as it must appear between the quotes of the canonical text.
std::string encode_key(const std::string& key) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(key.size());
    for (char c : key) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (u < 0x20) {
            out += "\\u00";
            out += hex[u >> 4];
            out += hex[u & 0xF];
        } else {
            out += c;
        }
    }
    return out;
}

WriteResult rejected(std::string why) { return {WriteResult::Status::Rejected, std::move(why)}; }

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
WriteResult set_value(JsonValue& root, std::string_view pointer, std::string_view value_json) {
    JsonValue value;
    const ParseConfig cfg;
    Parser parser(value_json, cfg);
    if (!parser.parse(value)) {
        return rejected("value is not JSON: " + parser.error());
    }
    std::vector<std::string> parts;
    if (!split_pointer(pointer, parts)) {
        return rejected("not a JSON pointer");
    }
    if (parts.empty()) {
        root = std::move(value);
        return {WriteResult::Status::Applied, ""};
    }
    JsonValue* parent = resolve(root, parts, parts.size() - 1);
    if (parent == nullptr) {
        return rejected("the parent does not exist");
    }
    const std::string& last = parts.back();
    if (parent->kind == JsonValue::Kind::Object) {
        auto it = find_member(parent->object, last);
        if (it != parent->object.end()) {
            it->second = std::move(value);
        } else {
            parent->object.emplace_back(encode_key(last), std::move(value));
        }
        return {WriteResult::Status::Applied, ""};
    }
    if (parent->kind == JsonValue::Kind::Array) {
        size_t idx = parent->array.size();
        if (last != "-" && !parse_index(last, idx)) {
            return rejected("not an array index");
        }
        if (idx == parent->array.size()) {
            parent->array.push_back(std::move(value));
        } else if (idx < parent->array.size()) {
            parent->array[idx] = std::move(value);
        } else {
            return rejected("array index past the end");
        }
        return {WriteResult::Status::Applied, ""};
    }
    return rejected("the parent is a scalar");
}

WriteResult remove_value(JsonValue& root, std::string_view pointer) {
    std::vector<std::string> parts;
    if (!split_pointer(pointer, parts)) {
        return rejected("not a JSON pointer");
    }
    if (parts.empty()) {
        return rejected("the root cannot be removed");
    }
    JsonValue* parent = resolve(root, parts, parts.size() - 1);
    if (parent == nullptr) {
        return rejected("the parent does not exist");
    }
    if (parent->kind == JsonValue::Kind::Object) {
        auto it = find_member(parent->object, parts.back());
        if (it == parent->object.end()) {
            return rejected("no such member");
        }
        parent->object.erase(it);
        return {WriteResult::Status::Applied, ""};
    }
    if (parent->kind == JsonValue::Kind::Array) {
        size_t idx = 0;
        if (!parse_index(parts.back(), idx) || idx >= parent->array.size()) {
            return rejected("no such element");
        }
        parent->array.erase(parent->array.begin() + static_cast<std::ptrdiff_t>(idx));
        return {WriteResult::Status::Applied, ""};
    }
    return rejected("the parent is a scalar");
}

// Mode 1: pretty, two-space indent, one member per line, empty containers stay "{}"/"[]".
// Scalars (and their number text) are emitted by the same code as the compact mode.
void serialize_pretty(const JsonValue& v, std::string& out, int indent) {
    const auto newline = [&out](int n) {
        out += '\n';
        out.append(static_cast<size_t>(n) * 2, ' ');
    };
    if (v.kind == JsonValue::Kind::Array && !v.array.empty()) {
        out += '[';
        for (size_t i = 0; i < v.array.size(); ++i) {
            if (i != 0) {
                out += ',';
            }
            newline(indent + 1);
            serialize_pretty(v.array[i], out, indent + 1);
        }
        newline(indent);
        out += ']';
    } else if (v.kind == JsonValue::Kind::Object && !v.object.empty()) {
        out += '{';
        for (size_t i = 0; i < v.object.size(); ++i) {
            if (i != 0) {
                out += ',';
            }
            newline(indent + 1);
            out += '"';
            out += v.object[i].first;
            out += "\": ";
            serialize_pretty(v.object[i].second, out, indent + 1);
        }
        newline(indent);
        out += '}';
    } else {
        serialize_value(v, out);
    }
}

} // namespace

struct ToySut::Impl {
    JsonValue root;
    bool has_root = false;
};

ToySut::ToySut() : impl_(std::make_unique<Impl>()) {}

ToySut::~ToySut() = default;

ParseResult ToySut::parse(std::string_view text, const ParseConfig& config) {
    Parser parser(text, config);
    JsonValue root;
    if (!parser.parse(root)) {
        return {false, parser.error()};
    }
    impl_->root = std::move(root);
    impl_->has_root = true;
    return {true, ""};
}

std::string ToySut::serialize() const {
    if (!impl_->has_root) {
        return "";
    }
    std::string out;
    serialize_value(impl_->root, out);
    return out;
}

std::vector<std::string> ToySut::pointers(int max_depth) const {
    std::vector<std::string> out;
    if (impl_->has_root) {
        collect_pointers(impl_->root, "", 0, max_depth, out);
    }
    return out;
}

std::optional<std::string> ToySut::get(std::string_view pointer) const {
    if (!impl_->has_root) {
        return std::nullopt;
    }
    return get_value(impl_->root, pointer);
}

WriteResult ToySut::set(std::string_view pointer, std::string_view value_json) {
    if (!impl_->has_root) {
        return rejected("nothing has been accepted");
    }
    return set_value(impl_->root, pointer, value_json);
}

WriteResult ToySut::remove(std::string_view pointer) {
    if (!impl_->has_root) {
        return rejected("nothing has been accepted");
    }
    return remove_value(impl_->root, pointer);
}

int ToySut::serialize_mode_count() const { return 2; }

std::string ToySut::serialize_mode(int mode) const {
    if (!impl_->has_root || mode < 0 || mode > 1) {
        return "";
    }
    std::string out;
    if (mode == 0) {
        serialize_value(impl_->root, out);
    } else {
        serialize_pretty(impl_->root, out, 0);
    }
    return out;
}

// Numbers are stored as their raw token and re-emitted byte-for-byte.
bool ToySut::preserves_number_text() const { return true; }

struct BrokenCommaSut::Impl {
    ToySut toy;
};

BrokenCommaSut::BrokenCommaSut() : impl_(std::make_unique<Impl>()) {}

BrokenCommaSut::~BrokenCommaSut() = default;

ParseResult BrokenCommaSut::parse(std::string_view text, const ParseConfig& config) {
    return impl_->toy.parse(text, config);
}

std::string BrokenCommaSut::serialize() const {
    std::string s = impl_->toy.serialize();
    if (!s.empty() && (s.back() == '}' || s.back() == ']')) {
        s.insert(s.size() - 1, ","); // "{...}" -> "{...,}"
    }
    return s;
}

std::vector<std::string> BrokenCommaSut::pointers(int max_depth) const {
    return impl_->toy.pointers(max_depth);
}

std::optional<std::string> BrokenCommaSut::get(std::string_view pointer) const {
    return impl_->toy.get(pointer);
}

namespace {

// A deliberately-broken SUT, registered as "broken-sibling": the toy, except that get() of
// an object member answers with the NEXT sibling's value (when there is one). Valid JSON,
// wrong member — the class O1/O2 cannot see — so the CLI `check` verb and the v0.3
// pointer-value law (O3) can be exercised end-to-end against a known-bad adapter. NOT used
// by the fuzz target.
class BrokenSiblingSut : public SystemUnderTest {
public:
    ParseResult parse(std::string_view text, const ParseConfig& config) override {
        Parser parser(text, config);
        JsonValue root;
        if (parser.parse(root)) {
            root_ = std::move(root);
        }
        return toy_.parse(text, config);
    }
    std::string serialize() const override { return toy_.serialize(); }
    std::vector<std::string> pointers(int max_depth) const override {
        return toy_.pointers(max_depth);
    }
    std::optional<std::string> get(std::string_view pointer) const override {
        std::vector<std::string> parts;
        if (split_pointer(pointer, parts) && !parts.empty()) {
            const JsonValue* parent = resolve(root_, parts, parts.size() - 1);
            if (parent != nullptr && parent->kind == JsonValue::Kind::Object) {
                auto it = find_member(parent->object, parts.back());
                if (it != parent->object.end() && std::next(it) != parent->object.end()) {
                    std::string s;
                    serialize_value(std::next(it)->second, s);
                    return s;
                }
            }
        }
        return toy_.get(pointer);
    }

private:
    ToySut toy_;
    JsonValue root_;
};

} // namespace

SutRegistry& default_registry() {
    static SutRegistry registry;
    return registry;
}

namespace {

// Register the toy SUT in the default registry on library load, so the CLI and tests can
// resolve it by name with no explicit wiring.
struct ToyRegistrar {
    ToyRegistrar() {
        default_registry().register_sut("toy", [] { return std::make_unique<ToySut>(); });
        default_registry().register_sut("broken-comma",
                                        [] { return std::make_unique<BrokenCommaSut>(); });
        default_registry().register_sut("broken-sibling",
                                        [] { return std::make_unique<BrokenSiblingSut>(); });
    }
};
ToyRegistrar toy_registrar;

} // namespace

} // namespace jsonfuzz
