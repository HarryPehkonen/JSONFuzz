#include <jsonfuzz/mutate.hpp>

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstring>
#include <utility>

namespace jsonfuzz {

namespace {

// A deterministic splitmix64 PRNG seeded by the operator's seed.
class Rng {
public:
    explicit Rng(uint64_t seed) : state_(seed) {}

    uint64_t next() {
        state_ += 0x9E3779B97F4A7C15ULL;
        uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    size_t pick(size_t n) { return n ? static_cast<size_t>(next() % n) : 0; }

private:
    uint64_t state_;
};

bool is_digit(char c) { return c >= '0' && c <= '9'; }

void collect_nodes(nlohmann::json& j, std::vector<nlohmann::json*>& out) {
    out.push_back(&j);
    if (j.is_array()) {
        for (auto& e : j) {
            collect_nodes(e, out);
        }
    } else if (j.is_object()) {
        for (auto& kv : j.items()) {
            collect_nodes(kv.value(), out);
        }
    }
}

// Truncate the document at a boundary. `boundary` is the set of characters that mark a
// cut point; `cut_after` keeps the boundary character in the prefix.
MutateResult truncate_at(const std::string& s, const std::string& boundary, bool cut_after,
                         Rng& rng) {
    std::vector<size_t> candidates;
    for (size_t i = 0; i < s.size(); ++i) {
        if (boundary.find(s[i]) != std::string::npos) {
            candidates.push_back(cut_after ? i + 1 : i);
        }
    }
    if (candidates.empty()) {
        return {s, false};
    }
    size_t cut = candidates[rng.pick(candidates.size())];
    if (cut > s.size()) {
        cut = s.size();
    }
    return {s.substr(0, cut), true};
}

MutateResult op_delete_member(const std::string& s, Rng& rng) {
    nlohmann::json j = nlohmann::json::parse(s);
    std::vector<nlohmann::json*> nodes;
    collect_nodes(j, nodes);
    std::vector<nlohmann::json*> objs;
    for (auto* n : nodes) {
        if (n->is_object() && !n->empty()) {
            objs.push_back(n);
        }
    }
    if (objs.empty()) {
        return {s, false};
    }
    auto* obj = objs[rng.pick(objs.size())];
    std::vector<std::string> keys;
    for (auto it = obj->begin(); it != obj->end(); ++it) {
        keys.push_back(it.key());
    }
    obj->erase(keys[rng.pick(keys.size())]);
    return {j.dump(), true};
}

MutateResult op_delete_element(const std::string& s, Rng& rng) {
    nlohmann::json j = nlohmann::json::parse(s);
    std::vector<nlohmann::json*> nodes;
    collect_nodes(j, nodes);
    std::vector<nlohmann::json*> arrs;
    for (auto* n : nodes) {
        if (n->is_array() && !n->empty()) {
            arrs.push_back(n);
        }
    }
    if (arrs.empty()) {
        return {s, false};
    }
    auto* arr = arrs[rng.pick(arrs.size())];
    arr->erase(arr->begin() + static_cast<std::ptrdiff_t>(rng.pick(arr->size())));
    return {j.dump(), true};
}

MutateResult op_duplicate_subtree(const std::string& s, Rng& rng) {
    nlohmann::json j = nlohmann::json::parse(s);
    std::vector<nlohmann::json*> nodes;
    collect_nodes(j, nodes);
    auto* n = nodes[rng.pick(nodes.size())];
    if (j.is_array()) {
        j.push_back(*n);
        return {j.dump(), true};
    }
    if (j.is_object()) {
        j["dup"] = *n;
        return {j.dump(), true};
    }
    return {s, false};
}

MutateResult op_change_value_kind(const std::string& s, Rng& rng) {
    nlohmann::json j = nlohmann::json::parse(s);
    std::vector<nlohmann::json*> nodes;
    collect_nodes(j, nodes);
    auto* n = nodes[rng.pick(nodes.size())];
    if (n->is_number()) {
        *n = n->dump();
    } else if (n->is_string()) {
        *n = 0;
    } else if (n->is_boolean()) {
        *n = n->get<bool>() ? 1 : 0;
    } else if (n->is_null()) {
        *n = "null";
    } else {
        return {s, false};
    }
    return {j.dump(), true};
}

MutateResult op_break_number(const std::string& s, Rng& rng) {
    std::vector<std::pair<size_t, size_t>> nums;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '-' && i + 1 < s.size() && is_digit(s[i + 1])) {
            // fall through to number scan
        } else if (!is_digit(s[i])) {
            ++i;
            continue;
        }
        const size_t start = i;
        if (s[i] == '-') {
            ++i;
        }
        while (i < s.size() && is_digit(s[i])) {
            ++i;
        }
        if (i < s.size() && s[i] == '.') {
            ++i;
            while (i < s.size() && is_digit(s[i])) {
                ++i;
            }
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
                ++i;
            }
            while (i < s.size() && is_digit(s[i])) {
                ++i;
            }
        }
        nums.emplace_back(start, i);
    }
    if (nums.empty()) {
        return {s, false};
    }
    const auto [st, en] = nums[rng.pick(nums.size())];
    static const char* broken[] = {"-01", "1.0.", "2.e+3"};
    std::string out = s.substr(0, st) + broken[rng.pick(3)] + s.substr(en);
    return {std::move(out), true};
}

MutateResult op_adversarial_escape(const std::string& s, Rng& rng) {
    std::vector<std::pair<size_t, size_t>> strs;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '"') {
            ++i;
            continue;
        }
        const size_t start = i + 1;
        size_t j = i + 1;
        while (j < s.size()) {
            if (s[j] == '\\') {
                j += 2;
                continue;
            }
            if (s[j] == '"') {
                break;
            }
            ++j;
        }
        if (j < s.size()) {
            strs.emplace_back(start, j);
        }
        i = j + 1;
    }
    if (strs.empty()) {
        return {s, false};
    }
    const auto [st, en] = strs[rng.pick(strs.size())];
    static const char* adv[] = {"\\ud800", "\\u0000", "\\uFFFF"};
    std::string out = s.substr(0, st) + adv[rng.pick(3)] + s.substr(en);
    return {std::move(out), true};
}

MutateResult op_unbalance(const std::string& s, Rng& rng) {
    std::vector<size_t> pos;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '{' || s[i] == '}' || s[i] == '[' || s[i] == ']') {
            pos.push_back(i);
        }
    }
    if (pos.empty()) {
        return {s, false};
    }
    const size_t p = pos[rng.pick(pos.size())];
    std::string out = s.substr(0, p) + s.substr(p + 1);
    return {std::move(out), true};
}

MutateResult op_mismatch_closer(const std::string& s, Rng& rng) {
    std::vector<size_t> pos;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '}' || s[i] == ']') {
            pos.push_back(i);
        }
    }
    if (pos.empty()) {
        return {s, false};
    }
    const size_t p = pos[rng.pick(pos.size())];
    std::string out = s;
    out[p] = (s[p] == '}') ? ']' : '}';
    return {std::move(out), true};
}

MutateResult op_inject_utf8(const std::string& s, Rng& rng) {
    if (s.empty()) {
        return {s, false};
    }
    const size_t p = rng.pick(s.size() + 1);
    std::string out = s.substr(0, p) + "\xff" + s.substr(p);
    return {std::move(out), true};
}

MutateResult op_swap_members(const std::string& s, Rng& rng) {
    nlohmann::json j = nlohmann::json::parse(s);
    std::vector<nlohmann::json*> nodes;
    collect_nodes(j, nodes);
    std::vector<nlohmann::json*> objs;
    for (auto* n : nodes) {
        if (n->is_object() && n->size() >= 2) {
            objs.push_back(n);
        }
    }
    if (objs.empty()) {
        return {s, false};
    }
    auto* obj = objs[rng.pick(objs.size())];
    std::vector<std::string> keys;
    for (auto it = obj->begin(); it != obj->end(); ++it) {
        keys.push_back(it.key());
    }
    size_t a = rng.pick(keys.size());
    size_t b = rng.pick(keys.size());
    if (a == b) {
        b = (b + 1) % keys.size();
    }
    auto va = (*obj)[keys[a]];
    auto vb = (*obj)[keys[b]];
    (*obj)[keys[a]] = vb;
    (*obj)[keys[b]] = va;
    return {j.dump(), true};
}

} // namespace

std::string_view name(Op op) {
    switch (op) {
    case Op::TruncateAtValue:
        return "truncate-at-value";
    case Op::TruncateAtKey:
        return "truncate-at-key";
    case Op::TruncateAtStructure:
        return "truncate-at-structure";
    case Op::TruncateAtEscape:
        return "truncate-at-escape";
    case Op::DeleteMember:
        return "delete-member";
    case Op::DeleteElement:
        return "delete-element";
    case Op::DuplicateSubtree:
        return "duplicate-subtree";
    case Op::WrapInArray:
        return "wrap-in-array";
    case Op::WrapInObject:
        return "wrap-in-object";
    case Op::ChangeValueKind:
        return "change-value-kind";
    case Op::BreakNumberGrammar:
        return "break-number-grammar";
    case Op::AdversarialEscape:
        return "adversarial-escape";
    case Op::UnbalanceBrackets:
        return "unbalance-brackets";
    case Op::MismatchCloser:
        return "mismatch-closer";
    case Op::InjectInvalidUtf8:
        return "inject-invalid-utf8";
    case Op::SwapMembers:
        return "swap-members";
    }
    return "unknown";
}

std::vector<Op> all_ops() {
    return {
        Op::TruncateAtValue,   Op::TruncateAtKey,   Op::TruncateAtStructure, Op::TruncateAtEscape,
        Op::DeleteMember,      Op::DeleteElement,   Op::DuplicateSubtree,    Op::WrapInArray,
        Op::WrapInObject,      Op::ChangeValueKind, Op::BreakNumberGrammar,  Op::AdversarialEscape,
        Op::UnbalanceBrackets, Op::MismatchCloser,  Op::InjectInvalidUtf8,   Op::SwapMembers};
}

MutateResult apply(std::string_view input, Op op, uint64_t seed) {
    std::string s(input);
    // Mutating an input that does not parse returns it unchanged and never throws.
    try {
        const auto parsed = nlohmann::json::parse(s);
        (void)parsed;
    } catch (...) {
        return {std::move(s), false};
    }
    Rng rng(seed);
    switch (op) {
    case Op::TruncateAtValue:
        return truncate_at(s, ":{[", true, rng);
    case Op::TruncateAtKey:
        return truncate_at(s, "{,", true, rng);
    case Op::TruncateAtStructure:
        return truncate_at(s, "{[,:", false, rng);
    case Op::TruncateAtEscape:
        return truncate_at(s, "\\", false, rng);
    case Op::DeleteMember:
        return op_delete_member(s, rng);
    case Op::DeleteElement:
        return op_delete_element(s, rng);
    case Op::DuplicateSubtree:
        return op_duplicate_subtree(s, rng);
    case Op::WrapInArray:
        return {"[" + s + "]", true};
    case Op::WrapInObject:
        return {"{\"k\":" + s + "}", true};
    case Op::ChangeValueKind:
        return op_change_value_kind(s, rng);
    case Op::BreakNumberGrammar:
        return op_break_number(s, rng);
    case Op::AdversarialEscape:
        return op_adversarial_escape(s, rng);
    case Op::UnbalanceBrackets:
        return op_unbalance(s, rng);
    case Op::MismatchCloser:
        return op_mismatch_closer(s, rng);
    case Op::InjectInvalidUtf8:
        return op_inject_utf8(s, rng);
    case Op::SwapMembers:
        return op_swap_members(s, rng);
    }
    return {std::move(s), false};
}

// The libFuzzer-shaped entry point; the signature is mandated by the brief, so the
// swappable-parameter warning is a false positive for a required API.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
size_t mutate(uint8_t* data, size_t size, size_t max_size, unsigned seed) {
    std::string s(size, '\0');
    for (size_t i = 0; i < size; ++i) {
        s[i] = static_cast<char>(data[i]);
    }
    const std::vector<Op> ops = all_ops();
    const Op op = ops[seed % ops.size()];
    MutateResult r = apply(s, op, seed);
    if (!r.changed) {
        return size;
    }
    if (r.text.size() > max_size) {
        r.text.resize(max_size);
    }
    for (size_t i = 0; i < r.text.size(); ++i) {
        data[i] = static_cast<uint8_t>(r.text[i]);
    }
    return r.text.size();
}

} // namespace jsonfuzz
