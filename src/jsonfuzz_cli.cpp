#include <jsonfuzz/core.hpp>
#include <jsonfuzz/generator.hpp>
#include <jsonfuzz/mutate.hpp>
#include <jsonfuzz/oracle.hpp>
#include <jsonfuzz/sut.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// The jsonfuzz CLI (brief §6). Verbs: gen, mutate, check; flags --version, --help,
// --list-ops, --list-suts. Exit codes: 0 ok, 1 an oracle violation, 2 usage error, 3 no
// such SUT. An unknown flag or verb is a usage error (exit 2) — JSOM once shipped a CLI
// that swallowed unknown options and exited 0, so the CLI must fail loudly on anything
// it does not document.

namespace {

void print_version() { std::printf("JSONFuzz %s\n", jsonfuzz::version().c_str()); }

void print_help() {
    print_version();
    std::printf("\n"
                "Usage: jsonfuzz <verb> [options]\n"
                "\n"
                "Verbs:\n"
                "  gen [--seed N] [--bytes N] [--max-depth N] [--max-members N]\n"
                "      [--max-string N] [--nesting-bias N] [--adversarial-weight N]\n"
                "      [--intent FILE]      generate a document to stdout; --intent writes\n"
                "                           the intent record (the oracle's independent truth)\n"
                "                           as a JSON sidecar\n"
                "  mutate --in FILE --op NAME --seed N\n"
                "                           apply one JSON-aware mutation operator to FILE,\n"
                "                           deterministically from the seed; result to stdout\n"
                "  check --sut NAME --in FILE\n"
                "                           run the two fixed-point laws against the SUT and\n"
                "                           print each violation; exit 1 on a violation\n"
                "\n"
                "Flags:\n"
                "  --version                print the version and exit\n"
                "  --list-ops               list the mutation operators and exit\n"
                "  --list-suts              list the registered SUTs and exit\n"
                "  --help, -h               this text\n"
                "\n"
                "Exit codes: 0 ok, 1 an oracle violation, 2 usage error, 3 no such SUT.\n");
}

int usage_error(const std::string& msg) {
    std::fprintf(stderr, "jsonfuzz: %s (try --help)\n", msg.c_str());
    return 2;
}

// Deterministic byte stream from a seed (splitmix64), the same shape the generator tests
// use, so `gen --seed N` is reproducible across runs and machines.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::vector<uint8_t> bytes_from_seed(uint64_t seed, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n);
    uint64_t state = seed;
    for (size_t i = 0; i < n; ++i) {
        state += 0x9E3779B97F4A7C15ULL;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        out.push_back(static_cast<uint8_t>(z >> 32));
    }
    return out;
}

bool to_int(const std::string& s, int& out) {
    if (s.empty()) {
        return false;
    }
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

// Parse `--name value` pairs from argv[start..argc). Unknown flags and missing values
// are usage errors. Returns false and sets `err` on the first problem.
bool parse_options(int argc, char** argv, int start, const std::vector<std::string>& known,
                   std::map<std::string, std::string>& out, std::string& err) {
    for (int i = start; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.size() < 2 || a[0] != '-' || a[1] != '-') {
            err = "unexpected argument: " + a;
            return false;
        }
        const std::string name = a.substr(2);
        bool ok = false;
        for (const auto& k : known) {
            if (k == name) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            err = "unknown option: " + a;
            return false;
        }
        if (i + 1 >= argc) {
            err = "missing value for " + a;
            return false;
        }
        out[name] = argv[++i];
    }
    return true;
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

void write_intent(const std::string& path, const jsonfuzz::Intent& it) {
    nlohmann::json j;
    j["object_count"] = it.object_count;
    j["array_count"] = it.array_count;
    j["string_count"] = it.string_count;
    j["number_count"] = it.number_count;
    j["bool_count"] = it.bool_count;
    j["null_count"] = it.null_count;
    j["max_depth_reached"] = it.max_depth_reached;
    j["total_members"] = it.total_members;
    j["has_duplicate_key"] = it.has_duplicate_key;
    j["has_empty_key"] = it.has_empty_key;
    j["has_pointer_key"] = it.has_pointer_key;
    j["has_lone_surrogate"] = it.has_lone_surrogate;
    j["has_control_char"] = it.has_control_char;
    j["has_boundary_number"] = it.has_boundary_number;
    std::ofstream f(path);
    f << j.dump(2) << '\n';
}

int cmd_gen(int argc, char** argv) {
    std::map<std::string, std::string> o;
    std::string err;
    if (!parse_options(argc, argv, 2,
                       {"intent", "seed", "bytes", "max-depth", "max-members", "max-string",
                        "nesting-bias", "adversarial-weight"},
                       o, err)) {
        return usage_error(err);
    }
    jsonfuzz::GenOptions g;
    int seed = 0;
    int bytes_len = 4096;
    std::string intent_file;
    if (o.count("intent")) {
        intent_file = o["intent"];
    }
    if (o.count("seed") && !to_int(o["seed"], seed)) {
        return usage_error("bad --seed value: " + o["seed"]);
    }
    if (o.count("bytes") && !to_int(o["bytes"], bytes_len)) {
        return usage_error("bad --bytes value: " + o["bytes"]);
    }
    if (o.count("max-depth") && !to_int(o["max-depth"], g.max_depth)) {
        return usage_error("bad --max-depth value: " + o["max-depth"]);
    }
    if (o.count("max-members") && !to_int(o["max-members"], g.max_members)) {
        return usage_error("bad --max-members value: " + o["max-members"]);
    }
    if (o.count("max-string") && !to_int(o["max-string"], g.max_string)) {
        return usage_error("bad --max-string value: " + o["max-string"]);
    }
    if (o.count("nesting-bias") && !to_int(o["nesting-bias"], g.nesting_bias)) {
        return usage_error("bad --nesting-bias value: " + o["nesting-bias"]);
    }
    if (o.count("adversarial-weight") && !to_int(o["adversarial-weight"], g.adversarial_weight)) {
        return usage_error("bad --adversarial-weight value: " + o["adversarial-weight"]);
    }
    const auto bytes = bytes_from_seed(static_cast<uint64_t>(seed), static_cast<size_t>(bytes_len));
    jsonfuzz::ByteSource src(bytes.data(), bytes.size());
    const jsonfuzz::Generated gen = jsonfuzz::generate(g, src);
    std::fwrite(gen.text.data(), 1, gen.text.size(), stdout);
    std::fputc('\n', stdout);
    if (!intent_file.empty()) {
        write_intent(intent_file, gen.intent);
    }
    return 0;
}

int cmd_mutate(int argc, char** argv) {
    std::map<std::string, std::string> o;
    std::string err;
    if (!parse_options(argc, argv, 2, {"in", "op", "seed"}, o, err)) {
        return usage_error(err);
    }
    if (!o.count("in") || !o.count("op")) {
        return usage_error("mutate needs --in FILE and --op NAME");
    }
    std::string text;
    if (!read_file(o["in"], text)) {
        return usage_error("cannot read --in file: " + o["in"]);
    }
    jsonfuzz::Op op = jsonfuzz::Op::TruncateAtValue;
    bool found = false;
    for (const auto candidate : jsonfuzz::all_ops()) {
        if (jsonfuzz::name(candidate) == o["op"]) {
            op = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        return usage_error("unknown operator: " + o["op"] + " (see --list-ops)");
    }
    uint64_t seed = 0;
    if (o.count("seed")) {
        int s = 0;
        if (!to_int(o["seed"], s)) {
            return usage_error("bad --seed value: " + o["seed"]);
        }
        seed = static_cast<uint64_t>(s);
    }
    const jsonfuzz::MutateResult r = jsonfuzz::apply(text, op, seed);
    std::fwrite(r.text.data(), 1, r.text.size(), stdout);
    std::fputc('\n', stdout);
    return 0;
}

int cmd_check(int argc, char** argv) {
    std::map<std::string, std::string> o;
    std::string err;
    if (!parse_options(argc, argv, 2, {"sut", "in"}, o, err)) {
        return usage_error(err);
    }
    if (!o.count("sut") || !o.count("in")) {
        return usage_error("check needs --sut NAME and --in FILE");
    }
    auto sut = jsonfuzz::default_registry().create(o["sut"]);
    if (!sut) {
        std::fprintf(stderr, "jsonfuzz: no such SUT: %s (see --list-suts)\n", o["sut"].c_str());
        return 3;
    }
    std::string text;
    if (!read_file(o["in"], text)) {
        return usage_error("cannot read --in file: " + o["in"]);
    }
    const jsonfuzz::ParseConfig cfg;
    const auto violations = jsonfuzz::check(*sut, text, cfg);
    for (const auto& v : violations) {
        std::printf("%s violation: %s\n", v.law == jsonfuzz::OracleViolation::Law::O1 ? "O1" : "O2",
                    v.message.c_str());
    }
    return violations.empty() ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage_error("no command given");
    }
    const std::string verb = argv[1];
    if (verb == "--version") {
        print_version();
        return 0;
    }
    if (verb == "--help" || verb == "-h") {
        print_help();
        return 0;
    }
    if (verb == "--list-ops") {
        for (const auto op : jsonfuzz::all_ops()) {
            std::printf("%s\n", std::string(jsonfuzz::name(op)).c_str());
        }
        return 0;
    }
    if (verb == "--list-suts") {
        for (const auto& n : jsonfuzz::default_registry().names()) {
            std::printf("%s\n", n.c_str());
        }
        return 0;
    }
    if (verb == "gen") {
        return cmd_gen(argc, argv);
    }
    if (verb == "mutate") {
        return cmd_mutate(argc, argv);
    }
    if (verb == "check") {
        return cmd_check(argc, argv);
    }
    return usage_error("unknown command: " + verb);
}
