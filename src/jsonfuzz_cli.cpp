#include <jsonfuzz/core.hpp>

#include <cstdio>
#include <cstring>

namespace {

void print_version() { std::printf("JSONFuzz %s\n", jsonfuzz::version().c_str()); }

void print_help() {
    print_version();
    std::printf("\n"
                "Usage: jsonfuzz [--version | --help]\n"
                "\n"
                "Phase A verbs only: --version and --help. The gen/mutate/check verbs\n"
                "arrive in Phase B.\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "jsonfuzz: no command given (try --help)\n");
        return 2;
    }
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_help();
            return 0;
        }
        std::fprintf(stderr, "jsonfuzz: unknown option: %s (try --help)\n", argv[i]);
        return 2;
    }
    return 0;
}
