#!/usr/bin/env bash
# Probe: JSONFuzz must configure as SOMEBODY ELSE'S SUBDIRECTORY.
#
# NOT a kit fix — this one is repo-local, and it holds a rule of ours: a consumer gets
# `JSONFuzz::core` and nothing else. The incident behind it (INCIDENTS.md, 2026-09-20): the
# `format` custom target was guarded only by `if(CLANG_FORMAT)`, so any consumer that has its own
# `format` target could not take JSONFuzz in at all —
#     add_custom_target cannot create target "format" because another target with the same name
#     already exists
# — and JSOM worked around it by hand-listing jsonfuzz_core's translation units, which silently
# drops a source the day one is added here. The workaround was worse than the bug.
#
# Called by the `kitprobes` stage as `<probe> <path-to-tools/ci.sh>`; the repo root is derived from
# that argument. Offline and fast: the consumer configure is pointed at a local nlohmann include
# directory, so nothing is fetched. If no local nlohmann can be found the probe FAILS LOUDLY rather
# than skipping — a check that reports nothing and passes is the failure this repo exists to fight.
set -uo pipefail

GATE=${1:-"$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/tools/ci.sh"}
# The stage calls `<probe> <gate> <repo-root>`; accept both, derive from the gate path if absent.
REPO=${2:-"$(cd "$(dirname "$GATE")/.." && pwd)"}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0
checks=0
ok()  { checks=$((checks + 1)); printf '  ok   %s\n' "$1"; }
bad() { checks=$((checks + 1)); fails=$((fails + 1)); printf '  FAIL %s\n' "$1"; }

# ---- 1. the guard is in the file, by name ------------------------------------------------
if grep -qE 'if\(JSONFUZZ_IS_TOP_LEVEL AND CLANG_FORMAT\)' "$REPO/CMakeLists.txt"; then
    ok "the format target is guarded by JSONFUZZ_IS_TOP_LEVEL (name-level)"
else
    bad "CMakeLists.txt does not guard add_custom_target(format ...) with JSONFUZZ_IS_TOP_LEVEL"
fi

# ---- a local nlohmann, so the probe needs no network -------------------------------------
NLOHMANN_DIR=""
for candidate in /usr/include /usr/local/include "$REPO/build/_deps/nlohmann_json-src/include"; do
    if [ -f "$candidate/nlohmann/json.hpp" ]; then NLOHMANN_DIR="$candidate"; break; fi
done
if [ -z "$NLOHMANN_DIR" ]; then
    printf '  FAIL no local nlohmann/json.hpp found — this probe will not fetch, and will not pass silently\n'
    printf 'PROBE FAILED (0 ok, 1 failed)\n'
    exit 1
fi
ok "using the local nlohmann at $NLOHMANN_DIR (no fetch)"

# ---- 2. a consumer with its OWN format target must configure -----------------------------
cat > "$WORK/main.cpp" <<'EOF'
#include <jsonfuzz/core.hpp>
int main() { return jsonfuzz::version().empty() ? 1 : 0; }
EOF

cat > "$WORK/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.16)
project(Consumer LANGUAGES CXX)
# The consumer's own developer target. This is the collision that started it.
add_custom_target(format COMMAND ${CMAKE_COMMAND} -E echo "CONSUMER_FORMAT_RAN")
# The binary dir is deliberately NOT named "jsonfuzz": add_subdirectory(..., <binary-dir>) turns
# that name into a make target of its own, so `cmake --build ... --target jsonfuzz` would
# "succeed" and read as a leaked target. Cost me one false failure while writing this probe.
add_subdirectory(${JSONFUZZ_SRC} _jsonfuzz_bin)
if(NOT TARGET JSONFuzz::core)
    message(FATAL_ERROR "PROBE: JSONFuzz::core is missing — the one thing a consumer is promised")
endif()
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE JSONFuzz::core)
EOF

if cmake -S "$WORK" -B "$WORK/build" -DJSONFUZZ_SRC="$REPO" \
        -DJSONFUZZ_NLOHMANN_DIR="$NLOHMANN_DIR" > "$WORK/configure.log" 2>&1; then
    ok "a consumer with its own 'format' target configures (this used to die here)"
else
    bad "configure failed — see: $(grep -m1 -A3 -iE 'CMake Error' "$WORK/configure.log" | tr '\n' ' ')"
fi

if grep -q "JSONFuzz::core is missing" "$WORK/configure.log" 2>/dev/null; then
    bad "JSONFuzz::core was not visible to the consumer"
elif [ -d "$WORK/build" ]; then
    ok "JSONFuzz::core resolves in the consumer's scope"
fi

# ---- 3. our developer targets must NOT exist in the consumer's build system ---------------
absent() { # absent <target> <label>
    if cmake --build "$WORK/build" --target "$1" > "$WORK/target-$1.log" 2>&1; then
        bad "'$1' EXISTS in the consumer's build system ($2)"
    else
        ok "'$1' is absent ($2)"
    fi
}
absent jsonfuzz       "the CLI is not built in somebody else's tree"
absent jsonfuzz_tests "no test binary leaks into a consumer"
absent fuzz_jsonfuzz  "no libFuzzer target leaks into a consumer"

# ---- 4. the consumer keeps its OWN format target ------------------------------------------
cmake --build "$WORK/build" --target format > "$WORK/format.log" 2>&1
if grep -q "CONSUMER_FORMAT_RAN" "$WORK/format.log"; then
    ok "'format' resolves to the consumer's target, not ours"
else
    bad "'format' does not run the consumer's own target — the name was hijacked or lost"
fi

# ---- 5. the configure-time target listing stays top-level only ----------------------------
if grep -q "Build targets —" "$WORK/configure.log"; then
    bad "JSONFuzz printed its target listing into the consumer's configure output"
else
    ok "the target listing stays top-level only"
fi

echo
if [ "$fails" -eq 0 ]; then
    printf 'PROBE VERIFIED (%d ok, 0 failed)\n' "$checks"
    exit 0
fi
printf 'PROBE FAILED (%d ok, %d failed)\n' "$((checks - fails))" "$fails"
exit 1
