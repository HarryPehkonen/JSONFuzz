#include "jsonfuzz_reference.hpp"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>

namespace jsonfuzz::detail {

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool in_number(char c) {
    return is_digit(c) || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-';
}

// The same overflow test nlohmann makes: an integer too large for 64 bits falls back to its
// double parse, and a double parse that is not finite is out_of_range.406. The program never
// calls setlocale, so strtod reads '.' as the decimal point, as nlohmann's own call does here.
bool overflows_double(const std::string& token) {
    return std::isinf(std::strtod(token.c_str(), nullptr));
}

// The escaped form of the tag as it is written inside a JSON string, and as nlohmann dumps it.
const std::string& escaped_tag() {
    static const std::string tag = std::string("\\u0000") + kNumberTagBody;
    return tag;
}

} // namespace

TaggedText tag_unmodelable_numbers(std::string_view t) {
    TaggedText out;
    out.text.reserve(t.size());
    bool in_string = false;
    for (std::size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (in_string) {
            out.text += c;
            if (c == '\\' && i + 1 < t.size()) {
                out.text += t[++i];
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            out.text += c;
            continue;
        }
        if (c == '-' || is_digit(c)) {
            std::size_t j = i;
            while (j < t.size() && in_number(t[j])) {
                ++j;
            }
            const std::string token(t.substr(i, j - i));
            if (overflows_double(token)) {
                out.text += '"';
                out.text += escaped_tag();
                out.text += token;
                out.text += '"';
                ++out.tagged;
            } else {
                out.text += token;
            }
            i = j - 1;
            continue;
        }
        out.text += c;
    }
    return out;
}

std::string untag_numbers(std::string_view dumped) {
    const std::string open = "\"" + escaped_tag();
    std::string out;
    std::size_t i = 0;
    while (i < dumped.size()) {
        const std::size_t at = dumped.find(open, i);
        if (at == std::string_view::npos) {
            break;
        }
        const std::size_t start = at + open.size();
        const std::size_t close = dumped.find('"', start);
        if (close == std::string_view::npos) {
            break;
        }
        out += dumped.substr(i, at - i);
        out += dumped.substr(start, close - start); // a number token holds no '"' or '\\'
        i = close + 1;
    }
    out += dumped.substr(i);
    return out;
}

} // namespace jsonfuzz::detail
