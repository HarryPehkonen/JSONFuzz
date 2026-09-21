#include <jsonfuzz/generator.hpp>

#include <cstring>
#include <vector>

namespace jsonfuzz {

namespace {

enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };

class Generator {
public:
    Generator(const GenOptions& options, ByteSource& bytes) : options_(options), bytes_(bytes) {}

    Generated run() {
        std::string text;
        gen_value(text, 0);
        return {std::move(text), intent_};
    }

private:
    const GenOptions& options_;
    ByteSource& bytes_;
    Intent intent_;

    // A deterministic value in [0, n) drawn from the byte stream. n <= 0 yields 0.
    int pick(int n) {
        if (n <= 0) {
            return 0;
        }
        return static_cast<int>(bytes_.next() % static_cast<uint8_t>(n));
    }

    bool pick_bool() { return (bytes_.next() & 1) != 0; }

    // True with probability adversarial_weight/100.
    bool adversarial() {
        if (options_.adversarial_weight <= 0) {
            return false;
        }
        if (options_.adversarial_weight >= 100) {
            return true;
        }
        return static_cast<int>(bytes_.next() % 100) < options_.adversarial_weight;
    }

    int pick_members() { return pick(options_.max_members + 1); }

    Kind pick_kind(int depth) {
        if (depth >= options_.max_depth) {
            // Only scalars at the depth limit.
            static const Kind scalars[] = {Kind::Null, Kind::Bool, Kind::Number, Kind::String};
            return scalars[pick(4)];
        }
        int w_null = 1;
        int w_bool = 1;
        int w_num = 2;
        int w_str = 3;
        int w_arr = 2 + options_.nesting_bias / 20;
        int w_obj = 2 + options_.nesting_bias / 20;
        const int total = w_null + w_bool + w_num + w_str + w_arr + w_obj;
        int r = pick(total);
        if (r < w_null) {
            return Kind::Null;
        }
        r -= w_null;
        if (r < w_bool) {
            return Kind::Bool;
        }
        r -= w_bool;
        if (r < w_num) {
            return Kind::Number;
        }
        r -= w_num;
        if (r < w_str) {
            return Kind::String;
        }
        r -= w_str;
        if (r < w_arr) {
            return Kind::Array;
        }
        return Kind::Object;
    }

    void gen_value(std::string& out, int depth) {
        if (depth > intent_.max_depth_reached) {
            intent_.max_depth_reached = depth;
        }
        switch (pick_kind(depth)) {
        case Kind::Null:
            out += "null";
            ++intent_.null_count;
            break;
        case Kind::Bool:
            out += pick_bool() ? "true" : "false";
            ++intent_.bool_count;
            break;
        case Kind::Number:
            gen_number(out);
            ++intent_.number_count;
            break;
        case Kind::String:
            gen_string(out);
            ++intent_.string_count;
            break;
        case Kind::Array:
            gen_array(out, depth);
            ++intent_.array_count;
            break;
        case Kind::Object:
            gen_object(out, depth);
            ++intent_.object_count;
            break;
        }
    }

    void gen_array(std::string& out, int depth) {
        out += '[';
        const int n = pick_members();
        for (int i = 0; i < n; ++i) {
            if (i != 0) {
                out += ',';
            }
            gen_value(out, depth + 1);
        }
        out += ']';
        intent_.total_members += n;
    }

    void gen_object(std::string& out, int depth) {
        out += '{';
        const int n = pick_members();
        std::vector<std::string> used_keys;
        for (int i = 0; i < n; ++i) {
            if (i != 0) {
                out += ',';
            }
            std::string key;
            gen_key(key, used_keys);
            out += '"';
            out += key;
            out += "\":";
            gen_value(out, depth + 1);
        }
        out += '}';
        intent_.total_members += n;
    }

    void gen_key(std::string& out, std::vector<std::string>& used) {
        if (adversarial() && pick(3) == 0) {
            intent_.has_empty_key = true;
            return;
        }
        if (adversarial() && pick(3) == 0 && !used.empty()) {
            out = used[pick(static_cast<int>(used.size()))];
            intent_.has_duplicate_key = true;
            return;
        }
        if (adversarial() && pick(3) == 0) {
            out = pick_bool() ? "/" : "~";
            intent_.has_pointer_key = true;
            return;
        }
        gen_string_body(out);
        used.push_back(out);
    }

    void gen_string(std::string& out) {
        out += '"';
        gen_string_body(out);
        out += '"';
    }

    void gen_string_body(std::string& out) {
        if (adversarial() && pick(4) == 0) {
            inject_adversarial_escape(out);
            return;
        }
        const int len = pick(options_.max_string + 1);
        for (int i = 0; i < len; ++i) {
            append_escaped(out, pick_char());
        }
    }

    void inject_adversarial_escape(std::string& out) {
        static const char* escapes[]
            = {"\\u0000", "\\ud800", "\\uDBFF", "\\uDC00", "\\uFFFF", "\\u0001", "\\u001f"};
        const char* e = escapes[pick(7)];
        out += e;
        if (e[2] == 'd' || e[2] == 'D') {
            intent_.has_lone_surrogate = true;
        } else if (e[2] == '0' || e[2] == 'F') {
            intent_.has_control_char = true;
        }
    }

    void gen_number(std::string& out) {
        if (adversarial() && pick(3) == 0) {
            static const char* boundary[] = {"0", "-0", "1e309", "1e-400", "12345678901234567890"};
            out += boundary[pick(5)];
            intent_.has_boundary_number = true;
            return;
        }
        if (pick_bool()) {
            out += '-';
        }
        const int int_digits = 1 + pick(6);
        if (int_digits == 1) {
            out += '0';
        } else {
            out += static_cast<char>('1' + pick(9));
            for (int i = 1; i < int_digits; ++i) {
                out += static_cast<char>('0' + pick(10));
            }
        }
        if (pick(3) == 0) {
            out += '.';
            const int frac = 1 + pick(4);
            for (int i = 0; i < frac; ++i) {
                out += static_cast<char>('0' + pick(10));
            }
        }
        if (pick(4) == 0) {
            out += 'e';
            if (pick_bool()) {
                out += '-';
            }
            out += static_cast<char>('0' + pick(10));
        }
    }

    char pick_char() {
        // '/' and '~' are in the general alphabet (not only the adversarial pointer-key
        // branch) so pointer keys are reachable even at adversarial_weight = 0, the
        // configuration the fuzz target uses (measured in B3: a get() sabotage on '/'
        // keys survived 10 s because no such key was produced at weight 0).
        static const char* chars
            = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-.,:;!?()[]{}~/";
        return chars[pick(static_cast<int>(std::strlen(chars)))];
    }

    void append_escaped(std::string& out, char c) {
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else {
            out += c;
        }
    }
};

} // namespace

Generated generate(const GenOptions& options, ByteSource& bytes) {
    Generator gen(options, bytes);
    return gen.run();
}

} // namespace jsonfuzz
