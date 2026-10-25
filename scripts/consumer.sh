#!/usr/bin/env bash
#
# consumer — a consumer of this repo gets `JSONFuzz::core` and NOTHING ELSE.
#
# Called from gate.toml as `[stage.consumer] cmd = "scripts/consumer.sh"`. The verdict
# vocabulary (ci_begin/ci_pass/ci_fail/ci_skip) is defined in scripts/gate-env.sh.
#
# Why this exists: the `format` custom target was once guarded only by `if(CLANG_FORMAT)`, so a
# consumer with its own `format` target could not take this repo in as a subdirectory at all —
# `add_custom_target cannot create target "format" because another target with the same name
# already exists` — and the only workaround left to that consumer was hand-listing
# jsonfuzz_core's translation units, which silently drops a source the day one is added here
# (INCIDENTS.md, 2026-09-20). The guards themselves live in CMakeLists.txt behind
# JSONFUZZ_IS_TOP_LEVEL; this stage is what notices when a NEW top-level-only artifact is added
# without one, and what holds the promise that only JSONFuzz::core crosses the boundary.
#
# History, so nobody deletes it on the old reasoning a second time: this check lived in
# tools/kit-probes/consumer-subdirectory.sh until 2026-10-06, when the gate became gate.toml and
# the kitprobes stage was removed because "a probe checks a FILE, and the file it checked
# (tools/ci.sh) is gone". The same sweep measured this probe as GREEN against the new entry
# point — it only used the gate path to find the repo root — and named scripts/consumer.sh as
# its new home. That file was never written, so from 2026-10-06 to 2026-10-25 the rule was
# guarded by nothing while two documents asserted otherwise. It ports unchanged.
set -uo pipefail
. "$(dirname "$0")/gate-env.sh"

run_stage() {
    ci_begin "consumer (a consumer gets JSONFuzz::core and nothing else)"

    local work="$CI_LOG_DIR/consumer-work"
    rm -rf "$work"; mkdir -p "$work"
    : > "$CI_LOG_DIR/consumer.log"
    local fails=0 checks=0

    ok()  { checks=$((checks + 1)); printf '  ok   %s\n' "$1" >> "$CI_LOG_DIR/consumer.log"; }
    bad() { checks=$((checks + 1)); fails=$((fails + 1)); printf '  FAIL %s\n' "$1" >> "$CI_LOG_DIR/consumer.log"; }

    # 1. the guard is in the file, by name.
    if grep -qE 'if\(JSONFUZZ_IS_TOP_LEVEL AND CLANG_FORMAT\)' "$REPO_ROOT/CMakeLists.txt"; then
        ok "the format target is guarded by JSONFUZZ_IS_TOP_LEVEL (name-level)"
    else
        bad "CMakeLists.txt does not guard add_custom_target(format ...) with JSONFUZZ_IS_TOP_LEVEL"
    fi

    # A local nlohmann keeps this check offline. Without one, say SKIP out loud: a check that
    # reports nothing and passes is the failure this repo exists to fight.
    local nlohmann_dir="" candidate
    for candidate in "${JSONFUZZ_NLOHMANN_DIR:-}" /usr/include /usr/local/include \
                     "$CI_BUILD_DIR/_deps/nlohmann_json-src/include"; do
        if [ -n "$candidate" ] && [ -f "$candidate/nlohmann/json.hpp" ]; then
            nlohmann_dir="$candidate"; break
        fi
    done
    if [ -z "$nlohmann_dir" ]; then
        printf '    no local nlohmann/json.hpp found; this check needs no network and will not pass silently\n'
        ci_skip consumer "no local nlohmann/json.hpp (set JSONFUZZ_NLOHMANN_DIR to one to run this check)"
    fi
    ok "using the local nlohmann at $nlohmann_dir (no fetch)"

    # 2. a consumer WITH ITS OWN format target must configure. This is the collision that started it.
    cat > "$work/main.cpp" <<'EOF'
#include <jsonfuzz/core.hpp>
int main() { return jsonfuzz::version().empty() ? 1 : 0; }
EOF
    cat > "$work/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.16)
project(Consumer LANGUAGES CXX)
# The consumer's own developer target: the name that used to collide.
add_custom_target(format COMMAND ${CMAKE_COMMAND} -E echo "CONSUMER_FORMAT_RAN")
# The binary dir is deliberately NOT named "jsonfuzz": add_subdirectory(..., <binary-dir>) turns
# that name into a make target of its own, so `--target jsonfuzz` below would "succeed" and read
# as a leaked target. That cost one false failure when this check was first written.
add_subdirectory(${JSONFUZZ_SRC} _jsonfuzz_bin)
if(NOT TARGET JSONFuzz::core)
    message(FATAL_ERROR "PROBE: JSONFuzz::core is missing — the one thing a consumer is promised")
endif()
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE JSONFuzz::core)
EOF

    if cmake -S "$work" -B "$work/build" -DJSONFUZZ_SRC="$REPO_ROOT" \
            -DJSONFUZZ_NLOHMANN_DIR="$nlohmann_dir" > "$work/configure.log" 2>&1; then
        ok "a consumer with its own 'format' target configures (this is where it used to die)"
    else
        bad "configure failed — $(grep -m1 -A3 -iE 'CMake Error' "$work/configure.log" | tr '\n' ' ')"
    fi

    if grep -q "JSONFuzz::core is missing" "$work/configure.log" 2>/dev/null; then
        bad "JSONFuzz::core was not visible to the consumer"
    elif [ -d "$work/build" ]; then
        ok "JSONFuzz::core resolves in the consumer's scope"
    fi

    # 3. our developer targets must not exist in the consumer's build system.
    absent() {  # absent <target> <why it must be absent>
        if cmake --build "$work/build" --target "$1" > "$work/target-$1.log" 2>&1; then
            bad "'$1' EXISTS in a consumer's build system ($2)"
        else
            ok "'$1' is absent ($2)"
        fi
    }
    absent jsonfuzz       "the CLI is not built in somebody else's tree"
    absent jsonfuzz_tests "no test binary leaks into a consumer"
    absent fuzz_jsonfuzz  "no libFuzzer target leaks into a consumer"

    # 4. the consumer keeps its OWN format target.
    cmake --build "$work/build" --target format > "$work/format.log" 2>&1
    if grep -q "CONSUMER_FORMAT_RAN" "$work/format.log"; then
        ok "'format' resolves to the consumer's target, not ours"
    else
        bad "'format' does not run the consumer's own target — the name was hijacked or lost"
    fi

    # 5. the configure-time target listing stays top-level only.
    if grep -q "Build targets —" "$work/configure.log"; then
        bad "JSONFuzz printed its target listing into the consumer's configure output"
    else
        ok "the target listing stays top-level only"
    fi

    if [ "$fails" -gt 0 ]; then
        grep -E '^  (ok|FAIL) ' "$CI_LOG_DIR/consumer.log" | sed 's/^/      /'
        ci_fail consumer "$fails of $checks consumer checks failed — a consumer cannot take this repo in (INCIDENTS.md 2026-09-20, 2026-10-25)" "$CI_LOG_DIR/consumer.log"
    fi
    sed 's/^/      /' "$CI_LOG_DIR/consumer.log"
    ci_pass consumer
}

run_stage "$@"
