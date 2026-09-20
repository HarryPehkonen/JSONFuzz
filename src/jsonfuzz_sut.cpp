#include <jsonfuzz/sut.hpp>

#include <cctype>
#include <cstdint>
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
        for (const auto& kv : v.object) {
            collect_pointers(kv.second, ptr + "/" + escape_pointer_token(kv.first), depth + 1,
                             max_depth, out);
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

std::optional<std::string> get_value(const JsonValue& root, std::string_view pointer) {
    if (pointer.empty()) {
        std::string s;
        serialize_value(root, s);
        return s;
    }
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < pointer.size()) {
        if (pointer[i] != '/') {
            return std::nullopt;
        }
        ++i;
        size_t j = pointer.find('/', i);
        if (j == std::string_view::npos) {
            j = pointer.size();
        }
        parts.push_back(unescape_pointer_token(std::string(pointer.substr(i, j - i))));
        i = j;
    }
    const JsonValue* cur = &root;
    for (const auto& part : parts) {
        if (cur->kind == JsonValue::Kind::Array) {
            if (!all_digits(part)) {
                return std::nullopt;
            }
            size_t idx = 0;
            for (char c : part) {
                idx = idx * 10 + static_cast<size_t>(c - '0');
            }
            if (idx >= cur->array.size()) {
                return std::nullopt;
            }
            cur = &cur->array[idx];
        } else if (cur->kind == JsonValue::Kind::Object) {
            bool found = false;
            for (const auto& kv : cur->object) {
                if (kv.first == part) {
                    cur = &kv.second;
                    found = true;
                    break;
                }
            }
            if (!found) {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
    }
    std::string s;
    serialize_value(*cur, s);
    return s;
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
    }
};
ToyRegistrar toy_registrar;

} // namespace

} // namespace jsonfuzz
